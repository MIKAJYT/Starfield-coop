using System.Collections.Generic;
using System.Text;

namespace Pulsar.Services;

// Reads the mod's join and reply codes (include/Network/JoinCode.h in the DLL, which makes
// them): "PSR-" + Crockford base32 with a CRC-16. Used to catch a typo before the game
// starts. Must stay byte-for-byte compatible with JoinCode.h.
public static class JoinCodeText
{
    public enum Kind { Host = 1, Reply = 2 }

    public sealed record Endpoint(int Via, string Address, int Port);   // Via: 1 LAN, 2 public (STUN), 3 router (UPnP)

    public sealed record Code(Kind Kind, IReadOnlyList<Endpoint> Endpoints);

    private const string Alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    private const int SecretBytes = 16;
    private const int TagBytes = 4;
    private const int MaxEndpoints = 6;

    public static ushort Crc16(IReadOnlyList<byte> data, int length)
    {
        ushort crc = 0xFFFF;
        for (int i = 0; i < length; i++)
        {
            crc ^= (ushort)(data[i] << 8);
            for (int b = 0; b < 8; b++)
                crc = (crc & 0x8000) != 0 ? (ushort)((crc << 1) ^ 0x1021) : (ushort)(crc << 1);
        }
        return crc;
    }

    // Upper case, without spaces and dashes, O read as 0 and I/L as 1, "PSR-" + groups of 5.
    public static string Normalize(string text)
    {
        var sb = new StringBuilder();
        foreach (var raw in text ?? "")
        {
            var ch = char.ToUpperInvariant(raw);
            if (ch is '-' or ' ' or '\t' or '\r' or '\n') continue;
            sb.Append(ch);
        }
        var s = sb.ToString();
        if (!s.StartsWith("PSR")) return s;
        s = s.Substring(3).Replace('O', '0').Replace('I', '1').Replace('L', '1');
        var outText = new StringBuilder("PSR");
        for (int i = 0; i < s.Length; i++)
        {
            if (i % 5 == 0) outText.Append('-');
            outText.Append(s[i]);
        }
        return outText.ToString();
    }

    public static Code? Decode(string text)
    {
        var s = Normalize(text).Replace("-", "");
        if (!s.StartsWith("PSR")) return null;
        var bytes = new List<byte>();
        uint acc = 0;
        int bits = 0;
        foreach (var ch in s.Substring(3))
        {
            int v = Alphabet.IndexOf(ch);
            if (v < 0) return null;
            acc = (acc << 5) | (uint)v;
            bits += 5;
            if (bits >= 8)
            {
                bytes.Add((byte)(acc >> (bits - 8)));
                bits -= 8;
            }
        }
        if (bytes.Count < 2 + TagBytes + 1 + 2) return null;
        ushort crc = (ushort)((bytes[^2] << 8) | bytes[^1]);
        if (Crc16(bytes, bytes.Count - 2) != crc) return null;
        if (bytes[0] != 1) return null;
        int p = 1;
        int kind = bytes[p++];
        if (kind != 1 && kind != 2) return null;
        int idBytes = kind == 1 ? SecretBytes : TagBytes;
        if (bytes.Count < p + idBytes + 1 + 2) return null;
        p += idBytes;
        int n = bytes[p++];
        if (n > MaxEndpoints || bytes.Count != p + n * 7 + 2) return null;
        var eps = new List<Endpoint>();
        for (int i = 0; i < n; i++, p += 7)
        {
            int via = bytes[p];
            if (via < 1 || via > 3) return null;
            var addr = $"{bytes[p + 1]}.{bytes[p + 2]}.{bytes[p + 3]}.{bytes[p + 4]}";
            int port = (bytes[p + 5] << 8) | bytes[p + 6];
            eps.Add(new Endpoint(via, addr, port));
        }
        return new Code((Kind)kind, eps);
    }
}
