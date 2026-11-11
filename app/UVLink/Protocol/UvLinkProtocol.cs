using System.Buffers.Binary;

namespace UVLink.Protocol;

// Miroir de firmware/src/config.h + Control.h (docs/protocol.md). Little-endian.
public static class UvLinkProtocol
{
    public static readonly Guid ServiceUuid = new("9b0e0001-6f3a-4d2b-8e5c-3a1f2c4d5e60");
    public static readonly Guid ControlUuid = new("9b0e0002-6f3a-4d2b-8e5c-3a1f2c4d5e60");
    public static readonly Guid StatusUuid  = new("9b0e0003-6f3a-4d2b-8e5c-3a1f2c4d5e60");
    public static readonly Guid AudioRxUuid = new("9b0e0004-6f3a-4d2b-8e5c-3a1f2c4d5e60");
    public static readonly Guid AudioTxUuid = new("9b0e0005-6f3a-4d2b-8e5c-3a1f2c4d5e60");
    public static readonly Guid EventUuid   = new("9b0e0006-6f3a-4d2b-8e5c-3a1f2c4d5e60");
    public static readonly Guid DataUuid    = new("9b0e0007-6f3a-4d2b-8e5c-3a1f2c4d5e60");

    public const byte CmdPing = 0x01, CmdGetInfo = 0x02;
    public const byte CmdSetVfo = 0x10, CmdGetVfo = 0x11, CmdSetRadio = 0x12, CmdPtt = 0x13;
    public const byte CmdMonitor = 0x14, CmdRecallCh = 0x15, CmdSetMode = 0x20, CmdSetAudio = 0x21, CmdCalAudio = 0x22, CmdSetAfGain = 0x23, CmdSetRssiCorr = 0x24, CmdGetRssiCorr = 0x25, CmdGetGps = 0x40, CmdSetPos = 0x41;
    public const byte CmdUnknown = 0xFF;

    public static byte[] GetGps() => new[] { CmdGetGps };
    // position manuelle de l'opérateur (distance / azimut) ; 0,0 = aucune
    public static byte[] SetPos(double lat, double lon)
    {
        var b = new byte[9];
        b[0] = CmdSetPos;
        BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(1), (int)Math.Round(lat * 1e5));
        BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(5), (int)Math.Round(lon * 1e5));
        return b;
    }

    // Modes sélectionnables (fw ≥ 0.9) : SARSAT et Sonde ne sont plus des modes, ils
    // tournent en phonie dès que le VFO est dans la bande 400-440 MHz. ModeIds
    // donne l'octet SET_MODE de chaque entrée ; ModeNameOf traduit l'octet du STATUS.
    // POCSAG (fw ≥ 0.13) : mode 5, ADC 32 kHz comme la phonie, audio plat forcé.
    // ISM 433/868 (fw ≥ 0.14) : mode 6, ADC 32 kHz, audio plat forcé, impulsions OOK -> rtl_433 dans l'appli.
    public static readonly string[] ModeNames = { "Phonie", "APRS", "SSTV", "POCSAG", "ISM" };
    public static readonly byte[]   ModeIds   = { 0, 1, 4, 5, 6 };
    public static string ModeNameOf(byte mode) => mode switch { 0 => "Phonie", 1 => "APRS", 4 => "SSTV", 5 => "POCSAG", 6 => "ISM", 2 => "Phonie (SARSAT auto)", 3 => "Phonie (sonde auto)", _ => $"mode {mode}" };

    public static byte[] Ping() => new[] { CmdPing };
    public static byte[] GetInfo() => new[] { CmdGetInfo };
    public static byte[] GetVfo(byte vfo) => new[] { CmdGetVfo, vfo };
    public static byte[] SetRadio(byte txVfo, bool dualWatch) => new[] { CmdSetRadio, txVfo, (byte)(dualWatch ? 1 : 0), (byte)0 };
    public static byte[] Ptt(bool on) => new[] { CmdPtt, (byte)(on ? 1 : 0) };
    public static byte[] Monitor(bool on) => new[] { CmdMonitor, (byte)(on ? 1 : 0) };
    public static byte[] SetMode(byte mode) => new[] { CmdSetMode, mode };
    public static readonly string[] AudioRxModeNames = { "Coupé", "Permanent", "Auto (squelch)" };
    // streamRx : 0 coupé, 1 permanent, 2 auto ; gain x1..x64 (0 = inchangé)
    // agc : 0 inchangé, 1 off, 2 on
    public static byte[] SetAudio(byte streamRx, byte gain, byte agc = 0) => new[] { CmdSetAudio, streamRx, gain, agc };
    // Calibration du gain RX : crête mesurée pendant `seconds`, puis gain fixe et AGC coupé (fin : CONTROL non sollicité [0x22, 1, gain])
    public static byte[] CalibrateAudio(byte seconds = 3) => new[] { CmdCalAudio, seconds };
    // Gain audio RX du poste (BK4819 REG_48, hôte H29) : 0 = réglage du poste, 1..78 (78 le plus fort)
    public static byte[] SetAfGain(byte gain) => new[] { CmdSetAfGain, gain };
    // Correction RSSI dBm par bande (7 x i8, bandes 50/108/137/174/350/400/470 MHz), mémorisée dans l'ESP
    public static readonly string[] RssiBandNames = { "50-108 MHz", "108-137 MHz", "137-174 MHz", "174-350 MHz", "350-400 MHz", "400-470 MHz", "470+ MHz" };
    public static byte[] SetRssiCorr(sbyte[] corr)
    {
        var b = new byte[8]; b[0] = CmdSetRssiCorr;
        for (int i = 0; i < 7; i++) b[1 + i] = (byte)(i < corr.Length ? corr[i] : 0);
        return b;
    }
    public static byte[] GetRssiCorr() => new[] { CmdGetRssiCorr };
    public static sbyte[]? ParseRssiCorr(ReadOnlySpan<byte> d)
    {
        if (d.Length < 8 || d[0] != CmdGetRssiCorr) return null;
        var r = new sbyte[7]; for (int i = 0; i < 7; i++) r[i] = (sbyte)d[1 + i]; return r;
    }
    public static byte[] RecallChannel(byte vfo, ushort ch)
    {
        var b = new byte[4];
        b[0] = CmdRecallCh; b[1] = vfo;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(2), ch);
        return b;
    }

    public static byte[] SetVfo(byte vfo, VfoParams p)
    {
        var b = new byte[19];
        b[0] = CmdSetVfo; b[1] = vfo;
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(2), p.RxHz);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(6), p.TxHz);
        b[10] = p.Modulation; b[11] = (byte)(p.Narrow ? 1 : 0);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(12), p.CtcssRx);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(14), p.CtcssTx);
        b[16] = p.Power; b[17] = p.Squelch; b[18] = p.Flags;
        return b;
    }

    public static VfoParams? ParseVfo(ReadOnlySpan<byte> d)
    {
        // d = réponse complète : cmd, vfo, puis 17 o (rx:u32, tx:u32, mod, nar, ctcss rx:u16, tx:u16, pwr, sq, flags) = 19 o
        // (2026-10-30 : le seuil était à 20, toute réponse GET_VFO était rejetée -> « VFO inconnu de l'ESP »)
        if (d.Length < 19 || d[0] != CmdGetVfo) return null;
        return new VfoParams(
            BinaryPrimitives.ReadUInt32LittleEndian(d[2..]),
            BinaryPrimitives.ReadUInt32LittleEndian(d[6..]),
            d[10], d[11] != 0,
            BinaryPrimitives.ReadUInt16LittleEndian(d[12..]),
            BinaryPrimitives.ReadUInt16LittleEndian(d[14..]),
            d[16], d[17], d[18]);
    }

    public static RadioStatus? ParseStatus(ReadOnlySpan<byte> d)
    {
        if (d.Length < 28) return null;     // 28 o (voir Control.h buildStatus)
        byte fl = d[2];
        return new RadioStatus(
            RadioPresent: d[1] != 0,
            Signal: (fl & 1) != 0, Tx: (fl & 2) != 0, DualWatch: (fl & 4) != 0,
            Monitor: (fl & 8) != 0, RxVfo: (fl >> 4) & 1, TxVfo: (fl >> 5) & 1,
            PttRequested: (fl & 64) != 0,
            SMeter: d[3],
            RssiDbm: BinaryPrimitives.ReadInt16LittleEndian(d[4..]),
            BatteryMv: BinaryPrimitives.ReadUInt16LittleEndian(d[6..]),
            Rx0: BinaryPrimitives.ReadUInt32LittleEndian(d[8..]),
            Tx0: BinaryPrimitives.ReadUInt32LittleEndian(d[12..]),
            Rx1: BinaryPrimitives.ReadUInt32LittleEndian(d[16..]),
            Tx1: BinaryPrimitives.ReadUInt32LittleEndian(d[20..]),
            Mode: d[24], GpsFix: (d[25] & 1) != 0,
            AdcDc: BinaryPrimitives.ReadUInt16LittleEndian(d[26..]),
            AdcSat: (fl & 128) != 0,
            RxGain: d.Length >= 30 ? d[28] : (byte)0,       // fw ≥ 0.6 : 30 o
            Agc: d.Length >= 30 && d[29] != 0,
            AfGain: d.Length >= 36 ? d[30] : (byte)0,       // fw ≥ 0.7 : 36 o
            AdcPeak: d.Length >= 36 ? BinaryPrimitives.ReadUInt16LittleEndian(d[31..]) : (ushort)0,
            AdcRms: d.Length >= 36 ? BinaryPrimitives.ReadUInt16LittleEndian(d[33..]) : (ushort)0,
            SatPct: d.Length >= 36 ? d[35] : (byte)0);
    }
}

public record VfoParams(uint RxHz, uint TxHz, byte Modulation, bool Narrow,
                        ushort CtcssRx, ushort CtcssTx, byte Power, byte Squelch, byte Flags);

public record RadioStatus(bool RadioPresent, bool Signal, bool Tx, bool DualWatch, bool Monitor,
                          int RxVfo, int TxVfo, bool PttRequested, int SMeter, short RssiDbm,
                          ushort BatteryMv, uint Rx0, uint Tx0, uint Rx1, uint Tx1, byte Mode,
                          bool GpsFix, ushort AdcDc, bool AdcSat, byte RxGain, bool Agc,
                          byte AfGain, ushort AdcPeak, ushort AdcRms, byte SatPct);
