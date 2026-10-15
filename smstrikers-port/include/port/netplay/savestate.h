/*
 * savestate.h — Game state snapshot and restore for rollback netcode.
 *
 * Rollback requires the ability to:
 *   1. Capture the entire deterministic game state into a byte buffer.
 *   2. Restore it byte-for-byte to rewind the simulation.
 *
 * The state includes:
 *   - RNG seed (nlRandom / nlDefaultSeed)
 *   - All character positions, velocities, animation states (10 characters)
 *   - Ball position, velocity, state
 *   - ODE physics world (all rigid bodies, contacts)
 *   - Game-level state (score, timer, possession, match state)
 *   - Input-derived state (FlickDetection ring buffers, pad state times)
 *   - FixedUpdateTask accumulators
 *
 * Audio and rendering state are NOT saved — they are not deterministic
 * and recover naturally after rollback.
 */

#ifndef PORT_NETPLAY_SAVESTATE_H
#define PORT_NETPLAY_SAVESTATE_H

#include "types.h"

#ifdef __cplusplus

#include <cstddef>

/* ------------------------------------------------------------------------ */
/*  Serialization buffer                                                     */
/* ------------------------------------------------------------------------ */

/*
 * A simple growable byte buffer for serialization.  Writes append data;
 * reads consume from the front.  No heap allocation during gameplay if
 * the buffer is pre-sized (reserve).
 */
class SaveStateBuffer {
public:
    SaveStateBuffer();
    ~SaveStateBuffer();

    /* Pre-allocate capacity. */
    void Reserve(size_t bytes);

    /* Reset write/read position without freeing memory. */
    void Reset();

    /* Seek the read position (defaults to 0 for rewinding). */
    void SeekRead(size_t pos = 0) { m_readPos = pos; }

    /* --- Writing (serialize) --- */
    void WriteBytes(const void* src, size_t n);

    void WriteU8 (u8  v) { WriteBytes(&v, 1); }
    void WriteS8 (s8  v) { WriteBytes(&v, 1); }
    void WriteU16(u16 v) { WriteBytes(&v, 2); }
    void WriteS16(s16 v) { WriteBytes(&v, 2); }
    void WriteU32(u32 v) { WriteBytes(&v, 4); }
    void WriteS32(s32 v) { WriteBytes(&v, 4); }
    void WriteF32(f32 v) { WriteBytes(&v, 4); }

    template<typename T>
    void WriteArray(const T* arr, size_t count) {
        WriteBytes(arr, count * sizeof(T));
    }

    /* --- Reading (deserialize) --- */
    bool ReadBytes(void* dst, size_t n);

    u8  ReadU8()  { u8  v = 0; ReadBytes(&v, 1); return v; }
    s8  ReadS8()  { s8  v = 0; ReadBytes(&v, 1); return v; }
    u16 ReadU16() { u16 v = 0; ReadBytes(&v, 2); return v; }
    s16 ReadS16() { s16 v = 0; ReadBytes(&v, 2); return v; }
    u32 ReadU32() { u32 v = 0; ReadBytes(&v, 4); return v; }
    s32 ReadS32() { s32 v = 0; ReadBytes(&v, 4); return v; }
    f32 ReadF32() { f32 v = 0; ReadBytes(&v, 4); return v; }

    template<typename T>
    bool ReadArray(T* arr, size_t count) {
        return ReadBytes(arr, count * sizeof(T));
    }

    /* --- Query --- */
    const u8* Data() const { return m_data; }
    size_t    Size() const { return m_writePos; }
    size_t    ReadPos() const { return m_readPos; }
    bool      IsReadComplete() const { return m_readPos >= m_writePos; }

    /* Simple Fletcher-32 checksum of the written data. */
    u32 Checksum() const;

private:
    void Grow(size_t needed);

    u8*    m_data;
    size_t m_capacity;
    size_t m_writePos;
    size_t m_readPos;
};

/* ------------------------------------------------------------------------ */
/*  Save / Restore interface                                                 */
/* ------------------------------------------------------------------------ */

/*
 * Save the complete deterministic game state into the buffer.
 * The buffer is Reset() before writing.
 * Returns true on success.
 */
bool SaveGameState(SaveStateBuffer* buf);

/*
 * Restore the complete deterministic game state from the buffer.
 * The buffer's read position is set to 0 before reading.
 * Returns true on success.
 */
bool LoadGameState(SaveStateBuffer* buf);

/*
 * Compute a checksum of the current game state without fully serializing.
 * Used for desync detection: both peers compute this after each confirmed
 * frame and compare.
 */
u32 ComputeGameStateChecksum();

/*
 * Write "offset name" lines describing the layout of the last SaveGameState,
 * used to label desync dumps.
 */
#include <cstdio>
void NetplayWriteStateFieldMap(FILE* f);
/* Name of the field of the last SaveGameState that holds byte `off`. */
const char* NetplayStateFieldAt(size_t off, size_t* fieldStart);
/* Raw dump of the game objects and clocks, for finding unsaved state. */
void NetplayWriteSyncDump(const char* binPath, const char* txtPath);
/* True between a goal and the next kickoff (cGame state GS_POST_GOAL). */
bool NetplayIsGoalCelebration();

/* Ends every character's animation cross-fade (see gamestate_serialize.cpp). */
void NetplayFlattenAnimBlends();

/*
 * Automated rollback determinism self-test:
 * Rewinds and resimulates N frames, comparing post-rollback state with
 * pre-rollback state byte-for-byte to guarantee 100% determinism.
 */
bool NetplayTestRollbackDeterminism(int framesToTest, char* outResult, size_t outResultSize);

#endif /* __cplusplus */

#endif /* PORT_NETPLAY_SAVESTATE_H */
