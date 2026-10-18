using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using CASCLib;

namespace CASCExplorer
{
    // Resolve only *actual FileDataIDs stored inside the chosen client M2 file*.
    // Names below are explicitly synthetic labels for browsing and export.
    internal static class ForeverM2Dependencies
    {
        private sealed class Ref
        {
            public string Category, Extension;
            public int FileDataID;
        }
        private const int MaxEntriesPerChunk = 150000;
        private static readonly HashSet<string> UInt32Chunks = new HashSet<string>
        {
            "SFID", // skin
            "TXID", // texture
            "BFID", // bone
            "SKID"  // skeleton
        };
        private static string Category(string chunk)
        {
            switch(chunk)
            {
                case "SFID": return "Skins (M2 SFID)";
                case "TXID": return "Textures (M2 TXID)";
                case "BFID": return "Bones (M2 BFID)";
                case "SKID": return "Skeleton (M2 SKID)";
                case "AFID": return "Animations (M2 AFID)";
                default: return "Other M2 references";
            }
        }
        private static string Extension(string chunk)
        {
            switch(chunk)
            {
                case "SFID": return ".skin";
                case "TXID": return ".blp";
                case "BFID": return ".bone";
                case "SKID": return ".skel";
                case "AFID": return ".anim";
                default: return ".bin";
            }
        }
        private static IEnumerable<Ref> ReadRefs(Stream stream)
        {
            if (!stream.CanSeek) throw new InvalidDataException("M2 file stream does not support seeking");
            using (var reader = new BinaryReader(stream, System.Text.Encoding.ASCII, true))
            {
                // Modern World of Warcraft M2 files consist of typed 8-byte-header chunks.
                long total = stream.Length;
                int chunks = 0;
                while (stream.Position + 8 <= total && ++chunks <= 4000)
                {
                    string tag = new string(reader.ReadChars(4));
                    uint size = reader.ReadUInt32();
                    long start = stream.Position;
                    long end = start + (long)size;
                    if (end > total || end < start)
                        throw new InvalidDataException("Malformed M2 chunk " + tag + " at " + start);

                    if (UInt32Chunks.Contains(tag) && size % 4 == 0 && size / 4 <= MaxEntriesPerChunk)
                    {
                        for (uint i = 0; i < size / 4; ++i)
                        {
                            uint fid = reader.ReadUInt32();
                            if (fid > 0 && fid <= int.MaxValue)
                                yield return new Ref { Category=Category(tag), Extension=Extension(tag), FileDataID=(int)fid };
                        }
                    }
                    else if (tag == "AFID" && size % 8 == 0 && size / 8 <= MaxEntriesPerChunk)
                    {
                        for (uint i = 0; i < size / 8; ++i)
                        {
                            reader.ReadUInt16(); // animation ID
                            reader.ReadUInt16(); // variation ID
                            uint fid = reader.ReadUInt32();
                            if (fid > 0 && fid <= int.MaxValue)
                                yield return new Ref { Category=Category(tag), Extension=".anim", FileDataID=(int)fid };
                        }
                    }
                    stream.Position = end;
                }
            }
        }

        // Called only when the user expands a Male/Female race folder to avoid
        // reading/decompressing hundreds of large M2 files during startup.
        internal static void PopulateOnExpand(TreeNode node, CASCHandler casc)
        {
            if (node == null || casc == null) return;
            var f = node.Tag as CASCFolder;
            if (f == null || (f.Name != "Male" && f.Name != "Female")) return;
            if (node.Parent?.Parent?.Parent == null ||
                !node.Parent.Parent.Parent.Name.StartsWith("Forever DB2", StringComparison.Ordinal)) return;
            if (f.Folders.Remove("Related assets (expand race to scan)") == false)
                return; // one attempt per tree build

            try
            {
                var root = casc.Root as WowRootHandler;
                if (root == null) return;

                CASCFile m2 = f.Files.Values.FirstOrDefault(x => x.Name.StartsWith("M2_FileDataID_", StringComparison.OrdinalIgnoreCase));
                if (m2 == null) return;
                int id = root.GetFileDataIdByHash(m2.Hash);
                if (id <= 0 || !root.FileExist(id)) return;

                var unique = new HashSet<string>(StringComparer.Ordinal);
                using (Stream stream = casc.OpenFile(id))
                {
                    foreach (var entry in ReadRefs(stream))
                    {
                        if (!root.FileExist(entry.FileDataID)) continue;
                        string composite = entry.Category + "|" + entry.FileDataID;
                        if (!unique.Add(composite)) continue;

                        if (!f.Folders.TryGetValue(entry.Category, out CASCFolder folder))
                            f.Folders[entry.Category] = folder = new CASCFolder(entry.Category);

                        ulong hash = root.GetHashByFileDataId(entry.FileDataID);
                        string displayName = "FileDataID_" + entry.FileDataID + entry.Extension;
                        if (!folder.Files.ContainsKey(displayName))
                            folder.Files[displayName] = new CASCFile(hash, "Forever_DB2_derived\\" + displayName);
                    }
                }
                if (unique.Count == 0)
                    f.Folders["No embedded M2 dependencies found (DB2 customization still separate)"] =
                        new CASCFolder("No embedded M2 dependencies found (DB2 customization still separate)");
                Logger.WriteLine("Forever M2 " + id + ": resolved " + unique.Count + " in-CASC dependencies.");
            }
            catch (Exception ex)
            {
                f.Folders["M2 dependency scan failed (see message)"] = new CASCFolder("M2 dependency scan failed (see message)");
                MessageBox.Show("Cannot read model dependencies:\r\n" + ex.Message,
                    "Forever M2 reference scanner", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }
    }
}
