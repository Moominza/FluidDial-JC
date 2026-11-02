// FluidDial Updater - firmware file checks. These are compatibility checks, not signatures: they keep the
// wrong kind of file (an M5Dial or CYD image, a text file, a truncated download) from being sent. The pendant
// repeats the important checks itself and refuses what does not belong to it.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace FluidDialUpdater
{
    public enum FirmwareKind { Unknown, FullControl, Diagnostic }

    public class FirmwareInfo
    {
        // The pendant's app slots are 3264 KB each (default_8MB.csv).
        public const int MaxImageBytes = 3264 * 1024;
        public const int MinImageBytes = 200 * 1024;
        public const string Marker = "FDJC-IMAGE-TARGET:JC8048W550C_I:ESP32-S3:V1";

        public string Path;
        public string FileName;
        public byte[] Data;
        public int Size;
        public string Sha256;
        public string Md5;
        public FirmwareKind Kind = FirmwareKind.Unknown;
        public string Version = "";      // from the manifest, else from the file name
        public string Commit = "";
        public bool InManifest;
        public bool Compatible;
        public string Verdict = "";      // why it is (not) compatible, shown to the operator

        public string KindText
        {
            get
            {
                if (Kind == FirmwareKind.FullControl) return "FULL CONTROL (operational pendant)";
                if (Kind == FirmwareKind.Diagnostic) return "DIAGNOSTIC - motion disabled";
                return "unknown kind";
            }
        }

        static bool Contains(byte[] hay, string needle)
        {
            return IndexOf(hay, Encoding.ASCII.GetBytes(needle)) >= 0;
        }

        static int IndexOf(byte[] hay, byte[] needle)
        {
            if (needle.Length == 0 || hay.Length < needle.Length) return -1;
            int last = hay.Length - needle.Length;
            byte first = needle[0];
            for (int i = 0; i <= last; i++)
            {
                if (hay[i] != first) continue;
                int j = 1;
                while (j < needle.Length && hay[i + j] == needle[j]) j++;
                if (j == needle.Length) return i;
            }
            return -1;
        }

        public static string Sha256Hex(byte[] data)
        {
            using (System.Security.Cryptography.SHA256 sha = System.Security.Cryptography.SHA256.Create())
            {
                return OtaClient.Hex(sha.ComputeHash(data));
            }
        }

        public static FirmwareInfo Analyze(string path, Manifest manifest)
        {
            FirmwareInfo f = new FirmwareInfo();
            f.Path = path;
            f.FileName = System.IO.Path.GetFileName(path);
            try
            {
                FileInfo fi = new FileInfo(path);
                if (!fi.Exists) { f.Verdict = "The file does not exist."; return f; }
                if (fi.Length > MaxImageBytes) { f.Size = (int)Math.Min(fi.Length, int.MaxValue); f.Verdict = "Rejected: larger than the pendant's 3264 KB application slot."; return f; }
                f.Data = File.ReadAllBytes(path);
            }
            catch (Exception ex)
            {
                f.Verdict = "The file cannot be read: " + ex.Message;
                return f;
            }
            f.Size = f.Data.Length;
            f.Sha256 = Sha256Hex(f.Data);
            f.Md5 = OtaClient.Md5Hex(f.Data);

            string lower = f.FileName.ToLowerInvariant();
            if (f.Size < MinImageBytes) { f.Verdict = "Rejected: too small to be a FluidDial-JC application image."; return f; }
            if (f.Data[0] != 0xE9) { f.Verdict = "Rejected: not an ESP32 application image (bad first byte). Use the fluiddial-jc-*.bin application file, not a bootloader or partition file."; return f; }
            int chip = f.Data[12] | (f.Data[13] << 8);
            if (chip != 9) { f.Verdict = "Rejected: built for a different chip (not ESP32-S3), for example an M5Dial or CYD image."; return f; }
            if (!Contains(f.Data, Marker))
            {
                f.Verdict = "Rejected: this is not a FluidDial-JC (Guition JC8048W550C_I) image. M5Dial and CYD firmware cannot be installed on this pendant.";
                return f;
            }
            if (lower.IndexOf("m5dial") >= 0 || lower.IndexOf("cyddial") >= 0)
            {
                f.Verdict = "Rejected: the file name says M5Dial/CYD.";
                return f;
            }

            if (Contains(f.Data, "FULL CONTROL TEST BUILD")) f.Kind = FirmwareKind.FullControl;
            else if (Contains(f.Data, "MOTION DISABLED.")) f.Kind = FirmwareKind.Diagnostic;

            ManifestEntry e = manifest == null ? null : manifest.Find(f.FileName);
            if (e != null)
            {
                if (!string.Equals(e.Sha256, f.Sha256, StringComparison.OrdinalIgnoreCase))
                {
                    f.Verdict = "Rejected: the file does not match the SHA-256 in the package manifest (damaged or modified download).";
                    return f;
                }
                f.InManifest = true;
                f.Version = e.Version;
                f.Commit = e.Commit;
                if (e.Kind == FirmwareKind.FullControl || e.Kind == FirmwareKind.Diagnostic)
                {
                    if (f.Kind != FirmwareKind.Unknown && f.Kind != e.Kind)
                    {
                        f.Verdict = "Rejected: the file contents do not match the kind declared in the manifest.";
                        return f;
                    }
                    f.Kind = e.Kind;
                }
            }
            else
            {
                System.Text.RegularExpressions.Match m = System.Text.RegularExpressions.Regex.Match(f.FileName, @"(\d+\.\d+\.\d+[-A-Za-z0-9.]*?)(?=-(fullcontrol|diagnostic)|\.bin)");
                if (m.Success) f.Version = m.Groups[1].Value;
            }

            f.Compatible = true;
            f.Verdict = f.InManifest
                ? "Compatible FluidDial-JC image, SHA-256 matches the package manifest."
                : "Compatible FluidDial-JC image (not listed in this package's manifest: check the SHA-256 against the release notes).";
            return f;
        }
    }

    public class ManifestEntry
    {
        public string File;
        public string Sha256;
        public FirmwareKind Kind;
        public string Version = "";
        public string Commit = "";
    }

    /// <summary>firmware\MANIFEST.txt: one line per image: SHA256 file kind=fullcontrol|diagnostic version=x commit=y</summary>
    public class Manifest
    {
        public List<ManifestEntry> Entries = new List<ManifestEntry>();
        public string Directory = "";

        public ManifestEntry Find(string file)
        {
            foreach (ManifestEntry e in Entries)
                if (string.Equals(e.File, file, StringComparison.OrdinalIgnoreCase)) return e;
            return null;
        }

        public static Manifest Load(string firmwareDir)
        {
            Manifest m = new Manifest();
            m.Directory = firmwareDir;
            string p = System.IO.Path.Combine(firmwareDir, "MANIFEST.txt");
            if (!File.Exists(p)) return m;
            foreach (string raw in File.ReadAllLines(p))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line.StartsWith("#")) continue;
                string[] t = line.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                if (t.Length < 3 || t[0].Length != 64) continue;
                ManifestEntry e = new ManifestEntry();
                e.Sha256 = t[0];
                e.File = t[1];
                for (int i = 2; i < t.Length; i++)
                {
                    if (t[i].StartsWith("kind=")) e.Kind = t[i].Substring(5) == "fullcontrol" ? FirmwareKind.FullControl : (t[i].Substring(5) == "diagnostic" ? FirmwareKind.Diagnostic : FirmwareKind.Unknown);
                    else if (t[i].StartsWith("version=")) e.Version = t[i].Substring(8);
                    else if (t[i].StartsWith("commit=")) e.Commit = t[i].Substring(7);
                }
                m.Entries.Add(e);
            }
            return m;
        }
    }
}
