#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

/*
 * Byte-pattern scanner with IDA-style wildcards.
 *
 * Pattern text: whitespace-separated tokens, e.g.
 *     "48 89 5C 24 18 56 57 ??"     (CE-style double question mark)
 *     "48 89 5C 24 18 56 57 ?"      (IDA-style single question mark)
 * Both '?' and '??' mean exactly one wildcard byte, so patterns copied from
 * either tool work unchanged. Two hex digits make one concrete byte.
 *
 * Matching is anchor-accelerated: the first concrete byte is located with
 * memchr, the full pattern is verified only on candidates. A full 186 MB
 * image scan of one pattern stays in the tens-of-milliseconds range.
 *
 * Scanning target: executable sections of the main module only (.text) -
 * every signature of interest lives there, and it skips ~150 MB of data.
 */

namespace Signatures
{
    struct Pattern
    {
        uint8_t Bytes[256] = {};
        bool    Mask[256]  = {};   // true = byte must match, false = wildcard
        size_t  Size       = 0;
        bool    Valid      = false;
    };

    struct KnownSignature
    {
        const char* Name;
        const char* Text;
        uint32_t    Rva;           // expected offset for the build this was
                                   // extracted from; a mismatch means the game
                                   // was updated and the table wants a refresh
    };

    // The signatures the DLL actually depends on, extracted and verified
    // unique against Ride-Win64-Shipping.exe 5.6.0-20702 rel-1.3.
    inline constexpr KnownSignature kTable[] =
    {
        { "ProcessEvent",
          "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 00 01 00 00 48 8D 6C 24 30 48 89 9D 28 01 00 00",
          0x1495860 },
        { "AddTorqueInRadians",
          "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 48 8B DA 48 8B F1 E8 A6 69 BE FD 48 83 7B 20 00",
          0x38A2130 },
        { "GetPhysicsAngularVelRad",
          "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 33 FF 49 8B F0 48 89 7C 24 58 48 8B DA 48 8B E9 E8 27 FA BB FD",
          0x38A37D0 },
        { "GetPhysicsLinearVelocity",
          "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 33 FF 49 8B F0 48 89 7C 24 58 48 8B DA 48 8B E9 E8 77 F9 BB FD",
          0x38A3880 },
        { "SetPhysicsAngularVelRad",
          "48 89 5C 24 18 56 57 41 56 48 83 EC 60 48 8B DA 4C 8B F1 E8 48 10 BE FD 48 83 7B 20 00",
          0x38A7A90 },
        { "SetPhysicsLinearVelocity",
          "48 89 5C 24 18 56 57 41 56 48 83 EC 60 48 8B DA 4C 8B F1 E8 08 0F BE FD 48 83 7B 20 00",
          0x38A7BD0 },
        { "WakeRigidBody",
          "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 33 FF 48 8B DA 48 89 7C 24 30 48 8B F1 E8 EF A5 BB FD",
          0x38A8C10 },
        { "K2_GetActorRotation",
          "4C 8B DC 57 48 81 EC 90 00 00 00 48 8B 42 20 45 33 C9 48 85 C0 49 8B F8 41 0F 95 C1 4C 03 C8 4C 89 4A 20",
          0x38E4C00 },
        { "GetControlRotation",
          "40 53 48 83 EC 40 48 8B 42 20 45 33 C9 48 85 C0 49 8B D8 41 0F 95 C1 4C 03 C8 4C 89 4A 20 48 8D 54 24 20 48 8B 01 FF 90 90 07 00 00 0F 10 00 0F 11 03",
          0x3C4CD20 },
    };

    // Parse "48 89 ?5 ??" style text. '?' and '??' both consume one wildcard
    // byte; anything that is not two hex digits or a question mark fails.
    inline bool Parse(const char* Text, Pattern& Out)
    {
        Out.Size  = 0;
        Out.Valid = false;
        if (!Text)
            return false;

        const char* P = Text;
        while (*P)
        {
            while (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n')
                ++P;
            if (!*P)
                break;

            const char* TokenStart = P;
            while (*P && *P != ' ' && *P != '\t' && *P != '\r' && *P != '\n')
                ++P;
            const size_t TokenLen = (size_t)(P - TokenStart);

            if (Out.Size >= 256)
                return false;

            const bool bWildcard = (TokenLen == 1 && TokenStart[0] == '?') ||
                                   (TokenLen == 2 && TokenStart[0] == '?' && TokenStart[1] == '?');
            if (bWildcard)
            {
                Out.Bytes[Out.Size] = 0;
                Out.Mask[Out.Size]  = false;
                ++Out.Size;
                continue;
            }

            if (TokenLen != 2)
                return false;

            auto Nibble = [](char C) -> int
            {
                if (C >= '0' && C <= '9') return C - '0';
                if (C >= 'a' && C <= 'f') return C - 'a' + 10;
                if (C >= 'A' && C <= 'F') return C - 'A' + 10;
                return -1;
            };
            const int Hi = Nibble(TokenStart[0]);
            const int Lo = Nibble(TokenStart[1]);
            if (Hi < 0 || Lo < 0)
                return false;

            Out.Bytes[Out.Size] = (uint8_t)((Hi << 4) | Lo);
            Out.Mask[Out.Size]  = true;
            ++Out.Size;
        }

        Out.Valid = (Out.Size > 0);
        return Out.Valid;
    }

    // First match of Pat inside [Data, Data+Size), or nullptr. At least one
    // concrete byte is required - an all-wildcard pattern matches everything
    // and is always a bug in the caller.
    inline const uint8_t* FindPattern(const uint8_t* Data, size_t Size, const Pattern& Pat)
    {
        if (!Pat.Valid || !Data || Pat.Size == 0 || Size < Pat.Size)
            return nullptr;

        size_t Anchor = 0;
        while (Anchor < Pat.Size && !Pat.Mask[Anchor])
            ++Anchor;
        if (Anchor == Pat.Size)
            return nullptr;

        const uint8_t  Want      = Pat.Bytes[Anchor];
        const size_t   MaxStart  = Size - Pat.Size;   // inclusive
        size_t         Start     = 0;

        while (Start <= MaxStart)
        {
            const size_t HayStart = Start + Anchor;
            const size_t HayEnd   = MaxStart + Anchor;   // inclusive
            if (HayStart > HayEnd)
                return nullptr;

            const void* Hit = memchr(Data + HayStart, Want, HayEnd - HayStart + 1);
            if (!Hit)
                return nullptr;

            const size_t Candidate = (size_t)((const uint8_t*)Hit - Data) - Anchor;

            bool bMatch = true;
            for (size_t I = 0; I < Pat.Size; ++I)
            {
                if (Pat.Mask[I] && Data[Candidate + I] != Pat.Bytes[I])
                {
                    bMatch = false;
                    break;
                }
            }
            if (bMatch)
                return Data + Candidate;

            Start = Candidate + 1;
        }
        return nullptr;
    }

    // Scan every executable section of the main module (the game EXE).
    inline const uint8_t* FindInImage(const Pattern& Pat)
    {
        const uint8_t* Base = (const uint8_t*)GetModuleHandleW(nullptr);
        if (!Base)
            return nullptr;

        const IMAGE_DOS_HEADER* Dos = (const IMAGE_DOS_HEADER*)Base;
        if (Dos->e_magic != IMAGE_DOS_SIGNATURE)
            return nullptr;

        const IMAGE_NT_HEADERS* Nt = (const IMAGE_NT_HEADERS*)(Base + Dos->e_lfanew);
        if (Nt->Signature != IMAGE_NT_SIGNATURE)
            return nullptr;

        const IMAGE_SECTION_HEADER* Sec = IMAGE_FIRST_SECTION(Nt);
        for (unsigned I = 0; I < Nt->FileHeader.NumberOfSections; ++I)
        {
            if (!(Sec[I].Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;

            const uint8_t* Begin = Base + Sec[I].VirtualAddress;
            const size_t   Len   = Sec[I].Misc.VirtualSize;
            if (const uint8_t* Hit = FindPattern(Begin, Len, Pat))
                return Hit;
        }
        return nullptr;
    }

    inline const void* FindInImage(const char* Text)
    {
        Pattern P;
        if (!Parse(Text, P))
            return nullptr;
        return FindInImage(P);
    }
}
