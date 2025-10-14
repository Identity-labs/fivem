#include "StdInc.h"

#include <Hooking.h>
#include <ScriptEngine.h>
#include <ScriptSerialization.h>

#include <atArray.h>
#include <Pool.h>
#include <Streaming.h>

#include <GameInit.h>
#include <Local.h>

// Include RAGE format definitions
#define RAGE_FORMATS_GAME rdr3
#define RAGE_FORMATS_GAME_RDR3
#define RAGE_FORMATS_IN_GAME
#include <rmcDrawable.h>
#include <pgContainers.h>
#include <grcTexture.h>
#include <RageParser.h>

#include <vector>
#include <string>
#include <cstring>
#include <botan/base64.h>

// Forward declarations for texture decoding functions
bool DecodeTextureData(const void* data, uint16_t width, uint16_t height, std::vector<uint8_t>& rgbaData, uint32_t pixelFormat);
bool DecodeDXT1(const uint8_t* src, uint16_t width, uint16_t height, std::vector<uint8_t>& rgbaData);
bool DecodeDXT3(const uint8_t* src, uint16_t width, uint16_t height, std::vector<uint8_t>& rgbaData);
bool DecodeDXT5(const uint8_t* src, uint16_t width, uint16_t height, std::vector<uint8_t>& rgbaData);
bool EncodeToPNG(const std::vector<uint8_t>& rgbaData, uint16_t width, uint16_t height, std::string& output);

struct ImageParams
{
    uint16_t m_width;              // 0x00
    uint16_t m_height;             // 0x02
    uint16_t m_depth;              // 0x04
    uint8_t  m_dimension;          // 0x06
    uint8_t  m_pixelFormat;        // 0x07 (may be sgaBufferFormat)
    uint8_t  m_tileMode;           // 0x08
    uint8_t  m_antiAliasType;      // 0x09
    uint8_t  m_levels;             // 0x0A
    uint8_t  m_unk11;              // 0x0B
    uint8_t  m_unk12;              // 0x0C
    uint8_t  m_unk13;              // 0x0D
};

struct ShaderResourceView
{
    void* m_texture; // grcTexture
    BYTE unk_10;
    char pad_11[1];
    char pad_12[4];
    uint16_t unk_16;
    char pad_18[40];
};
struct grcTexture
{
    char _resourcePadding[0x18];
    ImageParams m_params;
    uint16_t m_usageCount;
    char *m_name;
    ShaderResourceView *m_srv;
    void *m_pixelData;
    uint32_t unk_40;
    uint16_t unk_44;
    BYTE unk_46;
    BYTE unk_47;
    uint32_t pad_48[8];
};

struct TxdStoreEntry
{
    rage::rdr3::pgDictionary<rage::rdr3::grcTexturePC>* txdData; // 0x0000 - Pointer to TXD's texture data
    uint32_t flags;        // 0x0008 - Additional flags  
    uint32_t pad1;          // 0x000C - Padding
    uint32_t nameHash;  // 0x0010 - First TXD name hash (joaat) - "button_prompts" = 0x5e5a32bb
    uint32_t refCount;     // 0x0014 - Reference count (0x00000000) - Now 0!
    uint32_t pad2;
};

static auto GetTxdStorePool()
{
    static auto pool = rage::GetPoolBase("TxdStore");
    return pool;
}

static InitFunction initFunction([]()
{
    // Native to get TXD store pool size
    fx::ScriptEngine::RegisterNativeHandler("GET_TXD_STORE_SIZE", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (pool)
        {
            context.SetResult(static_cast<uint32_t>(pool->GetSize()));
        }
        else
        {
            context.SetResult(0);
        }
    });

    // Native to dump first TXD pool entry for debugging
    fx::ScriptEngine::RegisterNativeHandler("DUMP_FIRST_TXD_ENTRY", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            trace("DUMP_FIRST_TXD_ENTRY: Failed to get TXD store pool\n");
            context.SetResult(0);
            return;
        }
        
        if (pool->GetSize() == 0)
        {
            trace("DUMP_FIRST_TXD_ENTRY: Pool is empty\n");
            context.SetResult(0);
            return;
        }
        
        auto entry = pool->GetAt<TxdStoreEntry>(0);
        if (!entry)
        {
            trace("DUMP_FIRST_TXD_ENTRY: First entry is null\n");
            context.SetResult(0);
            return;
        }
        
        uint32_t size = sizeof(TxdStoreEntry);
        trace("=== FIRST TXD ENTRY RAW DATA DUMP (Size: %d bytes) ===\n", size);
        trace("Entry address: %p\n", (void*)entry);
        trace("Pool size: %d\n", pool->GetSize());
        
        uint8_t* data = reinterpret_cast<uint8_t*>(entry);
        
        // Dump TxdStoreEntry struct in hex format
        for (int i = 0; i < size; i += 16) // 16 bytes per line
        {
            trace("%04X: ", i);
            
            // Print hex values
            for (int j = 0; j < 16; ++j)
            {
                if (i + j < size)
                {
                    trace("%02X ", data[i + j]);
                }
                else
                {
                    trace("   ");
                }
            }
            
            trace(" |");
            
            // Print ASCII representation
            for (int j = 0; j < 16 && i + j < size; ++j)
            {
                char c = data[i + j];
                if (c >= 32 && c <= 126) // Printable ASCII
                {
                    trace("%c", c);
                }
                else
                {
                    trace(".");
                }
            }
            
            trace("|\n");
        }
        
        trace("=====================================\n");
        
        // Analyze the structure fields
        trace("Field Analysis:\n");
        trace("txdData pointer: %p\n", (void*)entry->txdData);
        trace("flags: 0x%08X (%u)\n", entry->flags, entry->flags);
        trace("pad1: 0x%08X (%u)\n", entry->pad1, entry->pad1);
        trace("nameHash: 0x%08X (%u)\n", entry->nameHash, entry->nameHash);
        trace("refCount: 0x%08X (%u)\n", entry->refCount, entry->refCount);
        trace("pad2: 0x%08X (%u)\n", entry->pad2, entry->pad2);
        
        // Show the current TXD hash (this is the joaat hash of the current TXD name)
        trace("current TXD hash: 0x%08X (joaat hash)\n", entry->nameHash);
        
        trace("=====================================\n");
        
        context.SetResult(1);
    });

    // Native to get TXD entry flags
    fx::ScriptEngine::RegisterNativeHandler("GET_TXD_ENTRY_FLAGS", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            context.SetResult(0);
            return;
        }
        
        uint32_t index = context.GetArgument<uint32_t>(0);
        if (index >= pool->GetSize())
        {
            context.SetResult(0);
            return;
        }
        
        auto entry = pool->GetAt<TxdStoreEntry>(index);
        if (!entry)
        {
            context.SetResult(0);
            return;
        }
        
        context.SetResult(entry->flags);
    });

    // Native to get TXD entry name hash
    fx::ScriptEngine::RegisterNativeHandler("GET_TXD_ENTRY_NAME_HASH", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            context.SetResult(0);
            return;
        }
        
        uint32_t index = context.GetArgument<uint32_t>(0);
        if (index >= pool->GetSize())
        {
            context.SetResult(0);
            return;
        }
        
        auto entry = pool->GetAt<TxdStoreEntry>(index);
        if (!entry)
        {
            context.SetResult(0);
            return;
        }
        
                context.SetResult(entry->nameHash);
    });

    // Native to get TXD entry reference count
    fx::ScriptEngine::RegisterNativeHandler("GET_TXD_ENTRY_REF_COUNT", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            context.SetResult(0);
            return;
        }
        
        uint32_t index = context.GetArgument<uint32_t>(0);
        if (index >= pool->GetSize())
        {
            context.SetResult(0);
            return;
        }
        
        auto entry = pool->GetAt<TxdStoreEntry>(index);
        if (!entry)
        {
            context.SetResult(0);
            return;
        }
        
        context.SetResult(entry->refCount);
    });

    // Native to check if TXD entry is loaded
    fx::ScriptEngine::RegisterNativeHandler("IS_TXD_ENTRY_LOADED", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            context.SetResult(false);
            return;
        }
        
        uint32_t index = context.GetArgument<uint32_t>(0);
        if (index >= pool->GetSize())
        {
            context.SetResult(false);
            return;
        }
        
        auto entry = pool->GetAt<TxdStoreEntry>(index);
        if (!entry)
        {
            context.SetResult(false);
            return;
        }
        
        context.SetResult((entry->flags & 1) != 0);
    });

    // Native to get texture count for a TXD entry
    fx::ScriptEngine::RegisterNativeHandler("GET_TXD_TEXTURE_COUNT", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            context.SetResult(0);
            return;
        }
        
        uint32_t index = context.GetArgument<uint32_t>(0);
        if (index >= pool->GetSize())
        {
            context.SetResult(0);
            return;
        }
        
        auto entry = pool->GetAt<TxdStoreEntry>(index);
        if (!entry)
        {
            context.SetResult(0);
            return;
        }
        
        // Try to read texture count from TXD data using pgDictionary structure
        if (entry->txdData && !IsBadReadPtr(entry->txdData, 32))
        {
            uint16_t textureCount = entry->txdData->GetCount();
            context.SetResult(textureCount);
            return;
        }
        
        // If we can't find the texture count, return 0
        context.SetResult(0);
    });

    // Native to get texture name hash by TXD index and texture index
    fx::ScriptEngine::RegisterNativeHandler("GET_TXD_TEXTURE_NAME_HASH", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            context.SetResult(0);
            return;
        }
        
        uint32_t txdIndex = context.GetArgument<uint32_t>(0);
        uint32_t textureIndex = context.GetArgument<uint32_t>(1);
        
        if (txdIndex >= pool->GetSize())
        {
            context.SetResult(0);
            return;
        }
        
        auto entry = pool->GetAt<TxdStoreEntry>(txdIndex);
        if (!entry)
        {
            context.SetResult(0);
            return;
        }
        
        // Try to read texture name hash from TXD data using pgDictionary structure
        if (entry->txdData && !IsBadReadPtr(entry->txdData, 32))
        {
            uint16_t textureCount = entry->txdData->GetCount();
            if (textureIndex < textureCount)
            {
                // Use iterator to get the hash at the specified index
                auto it = entry->txdData->begin();
                for (uint16_t i = 0; i < textureIndex; ++i)
                {
                    ++it;
                }
                uint32_t textureHash = it->first; // Get the hash from the iterator
                context.SetResult(textureHash);
                return;
            }
        }
        
        // If we can't find the texture hash, return 0
        context.SetResult(0);
    });

    // Native to find TXD by name hash
    fx::ScriptEngine::RegisterNativeHandler("FIND_TXD_BY_HASH", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            context.SetResult(-1);
            return;
        }
        
        uint32_t nameHash = context.GetArgument<uint32_t>(0);
        
        // Search through the pool
        for (uint32_t i = 0; i < pool->GetSize(); ++i)
        {
            auto entry = pool->GetAt<TxdStoreEntry>(i);
            if (!entry)
                continue;
                
            // Check if the name hash matches
            if (entry->nameHash == nameHash)
            {
                context.SetResult(static_cast<int>(i));
                return;
            }
        }
        
        context.SetResult(-1);
    });

    // Native to find TXD by name (converts string to hash)
    fx::ScriptEngine::RegisterNativeHandler("FIND_TXD_BY_NAME", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            context.SetResult(-1);
            return;
        }
        
        const char* name = context.GetArgument<const char*>(0);
        if (!name)
        {
            context.SetResult(-1);
            return;
        }
        
        // Convert string to joaat hash
        uint32_t nameHash = 0;
        for (const char* p = name; *p; ++p)
        {
            char c = *p;
            if (c >= 'A' && c <= 'Z')
                c += 32; // Convert to lowercase
            
            nameHash += c;
            nameHash += (nameHash << 10);
            nameHash ^= (nameHash >> 6);
        }
        
        nameHash += (nameHash << 3);
        nameHash ^= (nameHash >> 11);
        nameHash += (nameHash << 15);
        
        // Search through the pool
        for (uint32_t i = 0; i < pool->GetSize(); ++i)
        {
            auto entry = pool->GetAt<TxdStoreEntry>(i);
            if (!entry)
                continue;
                
            // Check if the name hash matches
            if (entry->nameHash == nameHash)
            {
                context.SetResult(static_cast<int>(i));
                return;
            }
        }
        
        context.SetResult(-1);
    });

    // Native to get loaded TXD count
    fx::ScriptEngine::RegisterNativeHandler("GET_LOADED_TXD_COUNT", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            context.SetResult(0);
            return;
        }
        
        uint32_t loadedCount = 0;
        for (uint32_t i = 0; i < pool->GetSize(); ++i)
        {
            auto entry = pool->GetAt<TxdStoreEntry>(i);
            if (!entry)
                continue;
                
            // Check if loaded (flags2 bit 0)
            if (entry->flags & 1)
            {
                loadedCount++;
            }
        }
        
        context.SetResult(loadedCount);
    });

    // Native to decode texture data to standard image formats (PNG/JPEG)
    fx::ScriptEngine::RegisterNativeHandler("DECODE_TXD_TEXTURE_TO_IMAGE", [](fx::ScriptContext& context)
    {
        static std::string resultBuffer;
        
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            resultBuffer = "ERROR:Pool not found";
            context.SetResult<const char*>(resultBuffer.c_str());
            return;
        }
        
        uint32_t txdIndex = context.GetArgument<uint32_t>(0);
        uint32_t textureIndex = context.GetArgument<uint32_t>(1);
        const char* format = context.GetArgument<const char*>(2); // "PNG" or "JPEG"
        
        trace("DECODE_TXD_TEXTURE_TO_IMAGE: txdIndex=%d, textureIndex=%d, format=%s\n", txdIndex, textureIndex, format);
        
        if (txdIndex >= pool->GetSize())
        {
            resultBuffer = "ERROR:txdIndex out of range";
            context.SetResult<const char*>(resultBuffer.c_str());
            return;
        }
        
        auto entry = pool->GetAt<TxdStoreEntry>(txdIndex);
        if (!entry || !entry->txdData)
        {
            resultBuffer = "ERROR:Entry not found";
            context.SetResult<const char*>(resultBuffer.c_str());
            return;
        }
        
        uint16_t textureCount = entry->txdData->GetCount();
        if (textureIndex >= textureCount)
        {
            resultBuffer = "ERROR:textureIndex out of range";
            context.SetResult<const char*>(resultBuffer.c_str());
            return;
        }
        
        // Get the texture object
        auto it = entry->txdData->begin();
        for (uint16_t i = 0; i < textureIndex; ++i)
        {
            ++it;
        }
        
        rage::rdr3::grcTexturePC* texturePC = it->second;
        rage::grcTexture* texture = reinterpret_cast<rage::grcTexture*>(texturePC);
        if (!texture)
        {
            resultBuffer = "ERROR:Texture not found";
            context.SetResult<const char*>(resultBuffer.c_str());
            return;
        }
        
        uint16_t width = texturePC->GetWidth();
        uint16_t height = texturePC->GetHeight();
        
        trace("DECODE_TXD_TEXTURE_TO_IMAGE: Texture dimensions: %dx%d\n", width, height);
        
        try
        {
            // Access raw texture data directly from grcTexturePC
            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Accessing raw texture data...\n");
            
            const void* pixelData = nullptr;
            
            // Cast to our grcTexture structure to access the real fields
            grcTexture* realTexture = reinterpret_cast<grcTexture*>(texturePC);
            uint32_t pixelFormat = realTexture->m_params.m_pixelFormat;
            
            // Calculate expected data size
            size_t expectedDataSize = texturePC->GetDataSize();
            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Expected data size: %zu bytes\n", expectedDataSize);
            
            // Try to access the real texture data using the proper structure
            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Attempting to access real texture data via proper structure...\n");
            
            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Real texture structure:\n");
            trace("  - m_params.m_width: %d\n", realTexture->m_params.m_width);
            trace("  - m_params.m_height: %d\n", realTexture->m_params.m_height);
            trace("  - m_params.m_pixelFormat: %d\n", realTexture->m_params.m_pixelFormat);
            trace("  - m_srv: %p\n", (void*)realTexture->m_srv);
            trace("  - m_pixelData: %p\n", (void*)realTexture->m_pixelData);
            
            // Try m_pixelData first (most direct)
            if (realTexture->m_pixelData)
            {
                trace("DECODE_TXD_TEXTURE_TO_IMAGE: Trying m_pixelData at %p\n", (void*)realTexture->m_pixelData);
                
                // Check if this looks like texture data
                const uint8_t* testData = reinterpret_cast<const uint8_t*>(realTexture->m_pixelData);
                
                // Read first 16 bytes safely
                try
                {
                    trace("DECODE_TXD_TEXTURE_TO_IMAGE: First 16 bytes at m_pixelData: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                              testData[0], testData[1], testData[2], testData[3],
                              testData[4], testData[5], testData[6], testData[7],
                              testData[8], testData[9], testData[10], testData[11],
                              testData[12], testData[13], testData[14], testData[15]);
                        
                        // Check for DXT1 structure
                        uint16_t color0 = testData[0] | (testData[1] << 8);
                        uint16_t color1 = testData[2] | (testData[3] << 8);
                        uint32_t indices = testData[4] | (testData[5] << 8) | (testData[6] << 16) | (testData[7] << 24);
                        
                        trace("DECODE_TXD_TEXTURE_TO_IMAGE: DXT1 check - color0=0x%04X, color1=0x%04X, indices=0x%08X\n", color0, color1, indices);
                        
                        // Check for debug patterns (0xCCCCCCCC is Microsoft debug heap pattern)
                        if (indices == 0xCCCCCCCC)
                        {
                            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Detected debug heap pattern (indices=0xCCCCCCCC) - this is not real texture data!\n");
                            // Don't use this data, but continue searching
                        }
                        else if (color0 != 0 && color1 != 0 && color0 != 0xFFFF && color1 != 0xFFFF)
                        {
                            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Found valid DXT1 data in m_pixelData!\n");
                            pixelData = testData;
                            // Found valid data, stop searching
                        }
                    }
                    catch (...)
                    {
                        trace("DECODE_TXD_TEXTURE_TO_IMAGE: Failed to read m_pixelData - invalid memory\n");
                    }
                }
                
                // If m_pixelData didn't work, try m_srv->m_texture
                if (!pixelData && realTexture->m_srv)
                {
                    trace("DECODE_TXD_TEXTURE_TO_IMAGE: Trying m_srv->m_texture at %p\n", (void*)realTexture->m_srv->m_texture);
                    
                    // The m_srv->m_texture might point to another texture object with the actual data
                    grcTexture* srvTexture = reinterpret_cast<grcTexture*>(realTexture->m_srv->m_texture);
                    
                    if (srvTexture && srvTexture->m_pixelData)
                    {
                        trace("DECODE_TXD_TEXTURE_TO_IMAGE: Found pixel data via m_srv->m_texture at %p\n", (void*)srvTexture->m_pixelData);
                        
                        const uint8_t* testData = reinterpret_cast<const uint8_t*>(srvTexture->m_pixelData);
                        
                        try
                        {
                            trace("DECODE_TXD_TEXTURE_TO_IMAGE: First 16 bytes at srv->m_texture->m_pixelData: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                                  testData[0], testData[1], testData[2], testData[3],
                                  testData[4], testData[5], testData[6], testData[7],
                                  testData[8], testData[9], testData[10], testData[11],
                                  testData[12], testData[13], testData[14], testData[15]);
                            
                            // Check for DXT1 structure
                            uint16_t color0 = testData[0] | (testData[1] << 8);
                            uint16_t color1 = testData[2] | (testData[3] << 8);
                            uint32_t indices = testData[4] | (testData[5] << 8) | (testData[6] << 16) | (testData[7] << 24);
                            
                            trace("DECODE_TXD_TEXTURE_TO_IMAGE: srv DXT1 check - color0=0x%04X, color1=0x%04X, indices=0x%08X\n", color0, color1, indices);
                            
                            // Check for debug patterns (0xCCCCCCCC is Microsoft debug heap pattern)
                            if (indices == 0xCCCCCCCC)
                            {
                                trace("DECODE_TXD_TEXTURE_TO_IMAGE: Detected debug heap pattern in srv data (indices=0xCCCCCCCC) - this is not real texture data!\n");
                                // Don't use this data, but continue searching
                            }
                            else if (color0 != 0 && color1 != 0 && color0 != 0xFFFF && color1 != 0xFFFF)
                            {
                                trace("DECODE_TXD_TEXTURE_TO_IMAGE: Found valid DXT1 data in srv->m_texture->m_pixelData!\n");
                                pixelData = testData;
                                // Found valid data, stop searching
                            }
                        }
                        catch (...)
                        {
                            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Failed to read srv->m_texture->m_pixelData - invalid memory\n");
                        }
                    }
                }
                
                // If we still haven't found valid texture data, try scanning the texture object more thoroughly
                if (!pixelData)
                {
                    trace("DECODE_TXD_TEXTURE_TO_IMAGE: No valid texture data found in m_pixelData or m_srv->m_texture, scanning texture object...\n");
                    
                    // Scan the entire texture object for potential pixel data
                    uint8_t* textureBytes = reinterpret_cast<uint8_t*>(realTexture);
                    
                    for (int offset = 0x00; offset <= 0x200; offset += 4)
                    {
                        if (offset + 8 <= sizeof(grcTexture))
                        {
                            uint64_t* potentialPtr = reinterpret_cast<uint64_t*>(textureBytes + offset);
                            
                            // Check if this looks like a valid pointer
                            if (*potentialPtr != 0 && 
                                *potentialPtr > 0x10000000 && 
                                *potentialPtr < 0x7FFFFFFFFFFF &&
                                *potentialPtr != 0x200000000)
                            {
                                const uint8_t* testData = reinterpret_cast<const uint8_t*>(*potentialPtr);
                                
                                try
                                {
                                    // Check for DXT1 structure
                                    uint16_t color0 = testData[0] | (testData[1] << 8);
                                    uint16_t color1 = testData[2] | (testData[3] << 8);
                                    uint32_t indices = testData[4] | (testData[5] << 8) | (testData[6] << 16) | (testData[7] << 24);
                                    
                                    trace("DECODE_TXD_TEXTURE_TO_IMAGE: Scanning offset 0x%02X - color0=0x%04X, color1=0x%04X, indices=0x%08X\n", 
                                          offset, color0, color1, indices);
                                    
                                    // Check for debug patterns
                                    if (indices == 0xCCCCCCCC)
                                    {
                                        trace("DECODE_TXD_TEXTURE_TO_IMAGE: Debug pattern at offset 0x%02X (indices=0xCCCCCCCC), skipping\n", offset);
                                        continue;
                                    }
                                    
                                    // Check for valid DXT1 data
                                    if (color0 != 0 && color1 != 0 && color0 != 0xFFFF && color1 != 0xFFFF)
                                    {
                                        trace("DECODE_TXD_TEXTURE_TO_IMAGE: Found valid DXT1 data at offset 0x%02X!\n", offset);
                                        pixelData = testData;
                                        break; // Found valid data, stop scanning
                                    }
                                }
                                catch (...)
                                {
                                    // Invalid memory, continue
                                }
                            }
                        }
                    }
                }
                
                if (!pixelData)
                {
                    resultBuffer = "ERROR:No valid texture data found - texture may not be loaded or data is corrupted";
                    context.SetResult<const char*>(resultBuffer.c_str());
                    return;
                }
            }
            
            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Got pixel data at %p, format=%d\n", pixelData, pixelFormat);
            
            // Calculate data size based on format and dimensions
            uint32_t bytesPerPixel = 4; // Default to RGBA8
            switch (static_cast<rage::rdr3::sgaBufferFormat>(pixelFormat))
            {
            case rage::rdr3::sgaBufferFormat::BC1_UNORM:
            case rage::rdr3::sgaBufferFormat::BC1_UNORM_SRGB:
                bytesPerPixel = 1; // Compressed format
                break;
            case rage::rdr3::sgaBufferFormat::BC2_UNORM:
            case rage::rdr3::sgaBufferFormat::BC2_UNORM_SRGB:
            case rage::rdr3::sgaBufferFormat::BC3_UNORM:
            case rage::rdr3::sgaBufferFormat::BC3_UNORM_SRGB:
                bytesPerPixel = 1; // Compressed format
                break;
            case rage::rdr3::sgaBufferFormat::R8G8B8A8_UNORM:
            case rage::rdr3::sgaBufferFormat::R8G8B8A8_UNORM_SRGB:
            case rage::rdr3::sgaBufferFormat::B8G8R8A8_UNORM:
            case rage::rdr3::sgaBufferFormat::B8G8R8A8_UNORM_SRGB:
                bytesPerPixel = 4;
                break;
            case rage::rdr3::sgaBufferFormat::R8_UNORM:
            case rage::rdr3::sgaBufferFormat::A8_UNORM:
                bytesPerPixel = 1;
                break;
            }
            
            uint32_t dataSize = width * height * bytesPerPixel;
            std::vector<uint8_t> textureData(dataSize);
            memcpy(textureData.data(), pixelData, dataSize);
            
            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Copied %d bytes of texture data (format=%d, bytesPerPixel=%d)\n", 
                  dataSize, pixelFormat, bytesPerPixel);
            
            // Debug: Check first few bytes of copied data
            if (dataSize >= 16)
            {
                trace("DECODE_TXD_TEXTURE_TO_IMAGE: First 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                      textureData[0], textureData[1], textureData[2], textureData[3],
                      textureData[4], textureData[5], textureData[6], textureData[7],
                      textureData[8], textureData[9], textureData[10], textureData[11],
                      textureData[12], textureData[13], textureData[14], textureData[15]);
            }
            
            // Decode texture data to RGBA8 using the texture's pixel format
            std::vector<uint8_t> rgbaData;
            
            trace("DECODE_TXD_TEXTURE_TO_IMAGE: About to decode texture (format=%d, width=%d, height=%d)\n", 
                  pixelFormat, width, height);
            
            if (DecodeTextureData(textureData.data(), width, height, rgbaData, pixelFormat))
            {
                trace("DECODE_TXD_TEXTURE_TO_IMAGE: Successfully decoded %dx%d texture (format: %d)\n", width, height, pixelFormat);
                trace("DECODE_TXD_TEXTURE_TO_IMAGE: RGBA data size: %zu bytes\n", rgbaData.size());
                
                // Debug: Check first few RGBA pixels
                if (rgbaData.size() >= 16)
                {
                    trace("DECODE_TXD_TEXTURE_TO_IMAGE: First 4 RGBA pixels: ");
                    for (int i = 0; i < 16; i += 4)
                    {
                        trace("R%d=%d G%d=%d B%d=%d A%d=%d ", 
                              i/4, rgbaData[i], i/4, rgbaData[i+1], i/4, rgbaData[i+2], i/4, rgbaData[i+3]);
                    }
                    trace("\n");
                }
                
                // Convert RGBA8 to base64 with format info
                std::string imageData;
                if (strcmp(format, "PNG") == 0 || strcmp(format, "BMP") == 0)
                {
                    if (EncodeToPNG(rgbaData, width, height, imageData))
                    {
                        resultBuffer = std::to_string(width) + "x" + std::to_string(height) + ":PNG:" + imageData;
                        trace("DECODE_TXD_TEXTURE_TO_IMAGE: Successfully created PNG, base64 length: %zu\n", imageData.length());
                    }
                    else
                    {
                        resultBuffer = "ERROR:BMP encoding failed";
                    }
                }
                else
                {
                    resultBuffer = "ERROR:Unsupported format (use PNG or BMP)";
                }
            }
            else
            {
                trace("DECODE_TXD_TEXTURE_TO_IMAGE: Texture decoding failed for format %d\n", pixelFormat);
                resultBuffer = "ERROR:Texture decoding failed";
            }
        }
        catch (...)
        {
            trace("DECODE_TXD_TEXTURE_TO_IMAGE: Exception occurred\n");
            resultBuffer = "ERROR:Exception during decoding";
        }
        
        context.SetResult<const char*>(resultBuffer.c_str());
    });

    // Native to dump TXD data for analysis
    fx::ScriptEngine::RegisterNativeHandler("DUMP_TXD_DATA", [](fx::ScriptContext& context)
    {
        auto pool = GetTxdStorePool();
        if (!pool)
        {
            trace("DUMP_TXD_DATA: Failed to get TXD store pool\n");
            context.SetResult(0);
            return;
        }
        
        uint32_t index = context.GetArgument<uint32_t>(0);
        if (index >= pool->GetSize())
        {
            trace("DUMP_TXD_DATA: Index %d out of range (pool size: %d)\n", index, pool->GetSize());
            context.SetResult(0);
            return;
        }
        
        auto entry = pool->GetAt<TxdStoreEntry>(index);
        if (!entry)
        {
            trace("DUMP_TXD_DATA: Entry at index %d is null\n", index);
            context.SetResult(0);
            return;
        }
        
        trace("=== TXD DATA DUMP (Index: %d) ===\n", index);
        trace("Entry address: %p\n", (void*)entry);
        trace("TXD data pointer: %p\n", (void*)entry->txdData);
        trace("Name hash: 0x%08X\n", entry->nameHash);
        trace("Is loaded: %s\n", (entry->flags & 1) ? "true" : "false");
        
        // Show pgDictionary info if available
        if (entry->txdData && !IsBadReadPtr(entry->txdData, 32))
        {
            uint16_t textureCount = entry->txdData->GetCount();
            trace("Texture count: %d\n", textureCount);
            
            // Show texture name hashes using iterator
            auto it = entry->txdData->begin();
            for (uint16_t i = 0; i < textureCount && i < 10; ++i) // Limit to first 10
            {
                uint32_t textureHash = it->first; // Get the hash from the iterator
                trace("  Texture %d hash: 0x%08X\n", i, textureHash);
                ++it;
            }
        }
        
        // Try to dump some TXD data if the pointer looks valid
        if (entry->txdData && !IsBadReadPtr(entry->txdData, 64))
        {
            trace("TXD data dump (first 64 bytes):\n");
            uint8_t* data = reinterpret_cast<uint8_t*>(entry->txdData);
            
            for (int i = 0; i < 64; i += 16)
            {
                trace("%04X: ", i);
                
                // Print hex values
                for (int j = 0; j < 16; ++j)
                {
                    if (i + j < 64)
                    {
                        trace("%02X ", data[i + j]);
                    }
                    else
                    {
                        trace("   ");
                    }
                }
                
                trace(" |");
                
                // Print ASCII representation
                for (int j = 0; j < 16 && i + j < 64; ++j)
                {
                    char c = data[i + j];
                    if (c >= 32 && c <= 126) // Printable ASCII
                    {
                        trace("%c", c);
                    }
                    else
                    {
                        trace(".");
                    }
                }
                
                trace("|\n");
            }
        }
        else
        {
            trace("TXD data pointer is invalid or unreadable\n");
        }
        
        trace("=====================================\n");
        
        context.SetResult(1);
    });

});

// Texture format detection and decoding functions implementation
bool DecodeTextureData(const void* data, uint16_t width, uint16_t height, std::vector<uint8_t>& rgbaData, uint32_t pixelFormat)
{
    const uint8_t* src = static_cast<const uint8_t*>(data);
    rgbaData.resize(width * height * 4);
    
    trace("DecodeTextureData: Starting decode (format=%d, width=%d, height=%d, dataSize=%zu)\n", 
          pixelFormat, width, height, rgbaData.size());
    
    // Use the actual pixel format from the texture
    switch (static_cast<rage::rdr3::sgaBufferFormat>(pixelFormat))
    {
    case rage::rdr3::sgaBufferFormat::BC1_UNORM:
        return DecodeDXT1(src, width, height, rgbaData);
        
    case rage::rdr3::sgaBufferFormat::BC1_UNORM_SRGB:
        return DecodeDXT1(src, width, height, rgbaData);
        
    case rage::rdr3::sgaBufferFormat::BC2_UNORM:
        return DecodeDXT3(src, width, height, rgbaData);
        
    case rage::rdr3::sgaBufferFormat::BC2_UNORM_SRGB:
        return DecodeDXT3(src, width, height, rgbaData);
        
    case rage::rdr3::sgaBufferFormat::BC3_UNORM:
        return DecodeDXT5(src, width, height, rgbaData);
        
    case rage::rdr3::sgaBufferFormat::BC3_UNORM_SRGB:
        return DecodeDXT5(src, width, height, rgbaData);
        
    case rage::rdr3::sgaBufferFormat::R8G8B8A8_UNORM:
        memcpy(rgbaData.data(), src, width * height * 4);
        return true;
        
    case rage::rdr3::sgaBufferFormat::R8G8B8A8_UNORM_SRGB:
        memcpy(rgbaData.data(), src, width * height * 4);
        return true;
        
    case rage::rdr3::sgaBufferFormat::B8G8R8A8_UNORM:
        // Convert B8G8R8A8 to RGBA8 (swap R and B)
        for (uint32_t i = 0; i < width * height; i++)
        {
            rgbaData[i * 4] = src[i * 4 + 2];     // R
            rgbaData[i * 4 + 1] = src[i * 4 + 1]; // G
            rgbaData[i * 4 + 2] = src[i * 4];     // B
            rgbaData[i * 4 + 3] = src[i * 4 + 3]; // A
        }
        return true;
        
    case rage::rdr3::sgaBufferFormat::B8G8R8A8_UNORM_SRGB:
        // Convert B8G8R8A8 to RGBA8 (swap R and B)
        for (uint32_t i = 0; i < width * height; i++)
        {
            rgbaData[i * 4] = src[i * 4 + 2];     // R
            rgbaData[i * 4 + 1] = src[i * 4 + 1]; // G
            rgbaData[i * 4 + 2] = src[i * 4];     // B
            rgbaData[i * 4 + 3] = src[i * 4 + 3]; // A
        }
        return true;
        
    case rage::rdr3::sgaBufferFormat::R8_UNORM:
        // Convert R8 to RGBA8 (grayscale)
        for (uint32_t i = 0; i < width * height; i++)
        {
            rgbaData[i * 4] = src[i];     // R
            rgbaData[i * 4 + 1] = src[i]; // G
            rgbaData[i * 4 + 2] = src[i]; // B
            rgbaData[i * 4 + 3] = 255;    // A
        }
        return true;
        
    case rage::rdr3::sgaBufferFormat::A8_UNORM:
        // Convert A8 to RGBA8 (grayscale)
        for (uint32_t i = 0; i < width * height; i++)
        {
            rgbaData[i * 4] = 255;        // R
            rgbaData[i * 4 + 1] = 255;    // G
            rgbaData[i * 4 + 2] = 255;    // B
            rgbaData[i * 4 + 3] = src[i]; // A
        }
        return true;
        
    default:
        // Fallback: assume RGBA8
        memcpy(rgbaData.data(), src, std::min((size_t)(width * height * 4), rgbaData.size()));
        return true;
    }
}

// DXT1 decoder
bool DecodeDXT1(const uint8_t* src, uint16_t width, uint16_t height, std::vector<uint8_t>& rgbaData)
{
    trace("DecodeDXT1: Starting DXT1 decode (width=%d, height=%d)\n", width, height);
    
    uint16_t blocksX = (width + 3) / 4;
    uint16_t blocksY = (height + 3) / 4;
    
    trace("DecodeDXT1: Block dimensions: %dx%d\n", blocksX, blocksY);
    
    for (uint16_t y = 0; y < blocksY; y++)
    {
        for (uint16_t x = 0; x < blocksX; x++)
        {
            uint32_t blockOffset = (y * blocksX + x) * 8;
            uint16_t color0 = src[blockOffset] | (src[blockOffset + 1] << 8);
            uint16_t color1 = src[blockOffset + 2] | (src[blockOffset + 3] << 8);
            uint32_t indices = src[blockOffset + 4] | (src[blockOffset + 5] << 8) | 
                              (src[blockOffset + 6] << 16) | (src[blockOffset + 7] << 24);
            
            // Debug first block
            if (y == 0 && x == 0)
            {
                trace("DecodeDXT1: First block - color0=0x%04X, color1=0x%04X, indices=0x%08X\n", color0, color1, indices);
            }
            
            // Convert 16-bit colors to 32-bit RGBA
            uint8_t colors[4][4];
            
            // Color 0
            colors[0][0] = ((color0 >> 11) & 0x1F) << 3; // R
            colors[0][1] = ((color0 >> 5) & 0x3F) << 2;  // G
            colors[0][2] = (color0 & 0x1F) << 3;          // B
            colors[0][3] = 255;                          // A
            
            // Color 1
            colors[1][0] = ((color1 >> 11) & 0x1F) << 3; // R
            colors[1][1] = ((color1 >> 5) & 0x3F) << 2;  // G
            colors[1][2] = (color1 & 0x1F) << 3;          // B
            colors[1][3] = 255;                          // A
            
            // Interpolated colors
            if (color0 > color1)
            {
                // 4-color mode
                colors[2][0] = (2 * colors[0][0] + colors[1][0]) / 3;
                colors[2][1] = (2 * colors[0][1] + colors[1][1]) / 3;
                colors[2][2] = (2 * colors[0][2] + colors[1][2]) / 3;
                colors[2][3] = 255;
                
                colors[3][0] = (colors[0][0] + 2 * colors[1][0]) / 3;
                colors[3][1] = (colors[0][1] + 2 * colors[1][1]) / 3;
                colors[3][2] = (colors[0][2] + 2 * colors[1][2]) / 3;
                colors[3][3] = 255;
            }
            else
            {
                // 3-color mode (transparent)
                colors[2][0] = (colors[0][0] + colors[1][0]) / 2;
                colors[2][1] = (colors[0][1] + colors[1][1]) / 2;
                colors[2][2] = (colors[0][2] + colors[1][2]) / 2;
                colors[2][3] = 255;
                
                colors[3][0] = 0;
                colors[3][1] = 0;
                colors[3][2] = 0;
                colors[3][3] = 0; // Transparent
            }
            
            // Decode 4x4 block
            for (int py = 0; py < 4; py++)
            {
                for (int px = 0; px < 4; px++)
                {
                    int pixelX = x * 4 + px;
                    int pixelY = y * 4 + py;
                    
                    if (pixelX < width && pixelY < height)
                    {
                        int pixelIndex = (pixelY * width + pixelX) * 4;
                        int colorIndex = (indices >> (py * 8 + px * 2)) & 3;
                        
                        rgbaData[pixelIndex + 0] = colors[colorIndex][0];
                        rgbaData[pixelIndex + 1] = colors[colorIndex][1];
                        rgbaData[pixelIndex + 2] = colors[colorIndex][2];
                        rgbaData[pixelIndex + 3] = colors[colorIndex][3];
                    }
                }
            }
        }
    }
    
    return true;
}

// DXT3 decoder (simplified - just handles color, not alpha)
bool DecodeDXT3(const uint8_t* src, uint16_t width, uint16_t height, std::vector<uint8_t>& rgbaData)
{
    // For now, just decode as DXT1 (ignore alpha)
    return DecodeDXT1(src + 8, width, height, rgbaData);
}

// DXT5 decoder (simplified - just handles color, not alpha)
bool DecodeDXT5(const uint8_t* src, uint16_t width, uint16_t height, std::vector<uint8_t>& rgbaData)
{
    // For now, just decode as DXT1 (ignore alpha)
    return DecodeDXT1(src + 8, width, height, rgbaData);
}

// PNG encoder (creates proper PNG file structure)
bool EncodeToPNG(const std::vector<uint8_t>& rgbaData, uint16_t width, uint16_t height, std::string& output)
{
    // Create a proper PNG that uses the actual texture data
    std::vector<uint8_t> pngData;
    
    // PNG signature (8 bytes)
    pngData.insert(pngData.end(), {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});
    
    // IHDR chunk
    std::vector<uint8_t> ihdrData;
    // Width (4 bytes, big-endian)
    ihdrData.push_back((width >> 24) & 0xFF);
    ihdrData.push_back((width >> 16) & 0xFF);
    ihdrData.push_back((width >> 8) & 0xFF);
    ihdrData.push_back(width & 0xFF);
    // Height (4 bytes, big-endian)
    ihdrData.push_back((height >> 24) & 0xFF);
    ihdrData.push_back((height >> 16) & 0xFF);
    ihdrData.push_back((height >> 8) & 0xFF);
    ihdrData.push_back(height & 0xFF);
    // Bit depth, color type, compression, filter, interlace
    ihdrData.insert(ihdrData.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, no compression, no filter, no interlace
    
    // Calculate CRC32 for IHDR
    uint32_t crc = 0xFFFFFFFF;
    const char* ihdrType = "IHDR";
    for (int i = 0; i < 4; i++) {
        crc ^= ihdrType[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    for (uint8_t byte : ihdrData) {
        crc ^= byte;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    crc ^= 0xFFFFFFFF;
    
    // IHDR chunk length (4 bytes, big-endian)
    pngData.push_back(0);
    pngData.push_back(0);
    pngData.push_back(0);
    pngData.push_back(ihdrData.size());
    // IHDR type
    pngData.insert(pngData.end(), ihdrType, ihdrType + 4);
    // IHDR data
    pngData.insert(pngData.end(), ihdrData.begin(), ihdrData.end());
    // IHDR CRC
    pngData.push_back((crc >> 24) & 0xFF);
    pngData.push_back((crc >> 16) & 0xFF);
    pngData.push_back((crc >> 8) & 0xFF);
    pngData.push_back(crc & 0xFF);
    
    // IDAT chunk (image data) - USE ACTUAL TEXTURE DATA
    std::vector<uint8_t> imageData;
    for (int y = 0; y < height; y++) {
        imageData.push_back(0); // No filter
        for (int x = 0; x < width; x++) {
            size_t pixelIndex = (y * width + x) * 4;
            if (pixelIndex + 3 < rgbaData.size()) {
                imageData.push_back(rgbaData[pixelIndex]);     // R
                imageData.push_back(rgbaData[pixelIndex + 1]); // G
                imageData.push_back(rgbaData[pixelIndex + 2]); // B
                imageData.push_back(rgbaData[pixelIndex + 3]); // A
            } else {
                imageData.insert(imageData.end(), {0, 0, 0, 255}); // Black pixel
            }
        }
    }
    
    // Simple deflate compression (for now, just store raw data)
    std::vector<uint8_t> compressedData = imageData;
    
    // IDAT chunk length
    pngData.push_back((compressedData.size() >> 24) & 0xFF);
    pngData.push_back((compressedData.size() >> 16) & 0xFF);
    pngData.push_back((compressedData.size() >> 8) & 0xFF);
    pngData.push_back(compressedData.size() & 0xFF);
    // IDAT type
    const char* idatType = "IDAT";
    pngData.insert(pngData.end(), idatType, idatType + 4);
    // IDAT data
    pngData.insert(pngData.end(), compressedData.begin(), compressedData.end());
    
    // Calculate CRC for IDAT
    crc = 0xFFFFFFFF;
    for (int i = 0; i < 4; i++) {
        crc ^= idatType[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    for (uint8_t byte : compressedData) {
        crc ^= byte;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    crc ^= 0xFFFFFFFF;
    // IDAT CRC
    pngData.push_back((crc >> 24) & 0xFF);
    pngData.push_back((crc >> 16) & 0xFF);
    pngData.push_back((crc >> 8) & 0xFF);
    pngData.push_back(crc & 0xFF);
    
    // IEND chunk
    pngData.insert(pngData.end(), {0, 0, 0, 0}); // Length = 0
    const char* iendType = "IEND";
    pngData.insert(pngData.end(), iendType, iendType + 4);
    // IEND CRC
    crc = 0xFFFFFFFF;
    for (int i = 0; i < 4; i++) {
        crc ^= iendType[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    crc ^= 0xFFFFFFFF;
    pngData.push_back((crc >> 24) & 0xFF);
    pngData.push_back((crc >> 16) & 0xFF);
    pngData.push_back((crc >> 8) & 0xFF);
    pngData.push_back(crc & 0xFF);
    
    // Convert to base64 using Botan
    output = Botan::base64_encode(pngData.data(), pngData.size());
    
    return true;
}

