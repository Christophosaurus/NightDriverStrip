//+--------------------------------------------------------------------------
//
// File:        jsonserializer.h
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of the NightDriver software project.
//
//    NightDriver is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    NightDriver is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Nightdriver.  It is normally found in copying.txt
//    If not, see <https://www.gnu.org/licenses/>.
//
// Description:
//
//    Declares classes for JSON (de)serialization of selected
//    properties of selected classes
//
// History:     Mar-29-2023         Rbergen      Created for NightDriverStrip
//
//---------------------------------------------------------------------------

#pragma once

#include <atomic>
#include <ArduinoJson.h>
#include "jsonbase.h"
#include "FastLED.h"

struct IJSONSerializable
{
    virtual bool SerializeToJSON(JsonObject& jsonObject) = 0;
    virtual bool DeserializeFromJSON(const JsonObjectConst& jsonObject) { return false; }
};

template <class E>
constexpr auto to_value(E e) noexcept
{
	return static_cast<std::underlying_type_t<E>>(e);
}

#if USE_PSRAM
    struct JsonPsramAllocator
    {
        void* allocate(size_t size) {
            return ps_malloc(size);
        }

        void deallocate(void* pointer) {
            free(pointer);
        }

        void* reallocate(void* ptr, size_t new_size) {
            return ps_realloc(ptr, new_size);
        }
    };

    typedef BasicJsonDocument<JsonPsramAllocator> AllocatedJsonDocument;

#else
    typedef DynamicJsonDocument AllocatedJsonDocument;
#endif

namespace ArduinoJson
{
    template <>
    struct Converter<CRGB>
    {
        static bool toJson(const CRGB& color, JsonVariant dst)
        {
            return dst.set((uint32_t)((color.r << 16) | (color.g << 8) | color.b));
        }

        static CRGB fromJson(JsonVariantConst src)
        {
            return CRGB(src.as<uint32_t>());
        }

        static bool checkJson(JsonVariantConst src)
        {
            return src.is<uint32_t>();
        }
    };

    template <>
    struct Converter<CRGBPalette16>
    {
        static bool toJson(const CRGBPalette16& palette, JsonVariant dst)
        {
            AllocatedJsonDocument doc(384);

            JsonArray colors = doc.to<JsonArray>();

            for (auto& color: palette.entries)
                colors.add(color);

            return dst.set(doc);
        }

        static CRGBPalette16 fromJson(JsonVariantConst src)
        {
            CRGB colors[16];
            int colorIndex = 0;

            JsonArrayConst componentsArray = src.as<JsonArrayConst>();
            for (JsonVariantConst value : componentsArray)
                colors[colorIndex++] = value.as<CRGB>();

            return CRGBPalette16(colors);
        }

        static bool checkJson(JsonVariantConst src)
        {
            return src.is<JsonArrayConst>() && src.as<JsonArrayConst>().size() == 16;
        }
    };
}

bool LoadJSONFile(const char *fileName, size_t& bufferSize, std::unique_ptr<AllocatedJsonDocument>& pJsonDoc);
bool SaveToJSONFile(const char *fileName, size_t& bufferSize, IJSONSerializable& object);
bool RemoveJSONFile(const char *fileName);

//

class JSONWriter
{
    struct WriterEntry
    {
        std::atomic_bool flag = false;
        std::function<void()> writer;

        WriterEntry(std::function<void()> writer) :
            writer(writer)
        {}

        WriterEntry(WriterEntry&& entry) : WriterEntry(entry.writer)
        {}
    };

    std::vector<WriterEntry> writers;

    TaskHandle_t writerTask;
    SemaphoreHandle_t writerSemaphore;
    StaticSemaphore_t writerSemaphoreBuffer;

  public:
    JSONWriter()
    {
        writerSemaphore = xSemaphoreCreateBinaryStatic(&writerSemaphoreBuffer);
        xSemaphoreTake(writerSemaphore, 0); // Make sure the semaphore is not set
        xTaskCreatePinnedToCore(WriterInvokerEntryPoint, "JSONWriter", 4096, (void *) this, NET_PRIORITY, &writerTask, NET_CORE);
    }

    ~JSONWriter()
    {
        vTaskDelete(writerTask);
        vSemaphoreDelete(writerSemaphore);
    }

    size_t RegisterWriter(std::function<void()> writer)
    {
        // Add the writer with its flag unset
        writers.emplace_back(writer);
        return writers.size() - 1;
    }

    void FlagWriter(size_t index)
    {
        // Check if we received a valid writer index
        if (index >= writers.size())
            return;

        writers[index].flag = true;

        // Wake up the writer invoker task if it's sleeping
        xSemaphoreGive(writerSemaphore);
    }

    static void WriterInvokerEntryPoint(void * pv)
    {
        JSONWriter * pObj = (JSONWriter *) pv;

        for(;;)
        {
            // Wait until we're woken up by a writer being flagged
            xSemaphoreTake(pObj->writerSemaphore, portMAX_DELAY);

            for (auto &entry : pObj->writers)
            {
                if (entry.flag)
                {
                    entry.writer();
                    entry.flag = false;
                }
            }
        }
    }
};

extern DRAM_ATTR std::unique_ptr<JSONWriter> g_ptrJSONWriter;
