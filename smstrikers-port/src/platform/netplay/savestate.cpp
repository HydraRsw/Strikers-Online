/*
 * savestate.cpp — Serialization buffer implementation.
 */

#include "port/netplay/savestate.h"
#include <cstdlib>
#include <cstring>

/* ------------------------------------------------------------------------ */
/*  SaveStateBuffer                                                          */
/* ------------------------------------------------------------------------ */

SaveStateBuffer::SaveStateBuffer()
    : m_data(nullptr), m_capacity(0), m_writePos(0), m_readPos(0)
{
}

SaveStateBuffer::~SaveStateBuffer()
{
    free(m_data);
}

void SaveStateBuffer::Reserve(size_t bytes)
{
    if (bytes > m_capacity)
    {
        m_data = static_cast<u8*>(realloc(m_data, bytes));
        m_capacity = bytes;
    }
}

void SaveStateBuffer::Reset()
{
    m_writePos = 0;
    m_readPos  = 0;
}

void SaveStateBuffer::Grow(size_t needed)
{
    size_t required = m_writePos + needed;
    if (required <= m_capacity)
        return;

    /* Double or jump to required, whichever is larger. */
    size_t newCap = m_capacity ? m_capacity * 2 : 4096;
    if (newCap < required)
        newCap = required;

    m_data = static_cast<u8*>(realloc(m_data, newCap));
    m_capacity = newCap;
}

void SaveStateBuffer::WriteBytes(const void* src, size_t n)
{
    Grow(n);
    memcpy(m_data + m_writePos, src, n);
    m_writePos += n;
}

bool SaveStateBuffer::ReadBytes(void* dst, size_t n)
{
    if (m_readPos + n > m_writePos)
        return false;
    memcpy(dst, m_data + m_readPos, n);
    m_readPos += n;
    return true;
}

u32 SaveStateBuffer::Checksum() const
{
    /* Fletcher-32 over the written data. */
    u32 sum1 = 0xFFFF;
    u32 sum2 = 0xFFFF;

    const u8* p = m_data;
    size_t len = m_writePos;

    while (len > 0)
    {
        /* Process in blocks of up to 360 bytes to avoid overflow. */
        size_t blockLen = len > 360 ? 360 : len;
        len -= blockLen;

        while (blockLen > 0)
        {
            sum1 += *p++;
            sum2 += sum1;
            --blockLen;
        }

        sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
        sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
    }

    sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
    sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);

    return (sum2 << 16) | sum1;
}
