#!/usr/bin/env python3
"""Patch CASCExplorer to choose the actual local WoW Forever install.
Usage: py -3 apply_forever_patch.py <path-to-CASCExplorer-repository>
"""
from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
if not (root / "CASCExplorer.sln").is_file():
    raise SystemExit("ERROR: CASCExplorer.sln is missing")

def patch(path, edits):
    f = root / path
    if not f.is_file():
        raise SystemExit("Missing source file: " + path + "; run git submodule update --init --recursive")
    raw = f.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    src = raw.decode('utf-8-sig').replace('\r\n', '\n')
    for old, new in edits:
        if src.count(old) != 1:
            raise SystemExit(path + ": expected one exact match, found " + str(src.count(old)) +
                             "\nUpstream layout changed; refusing to generate a broken build.")
        src = src.replace(old, new, 1)
    newline = '\r\n' if b'\r\n' in raw else '\n'
    f.write_bytes(src.replace('\n', newline).encode('utf-8-sig' if bom else 'utf-8'))
    print("PATCHED:", path)

reader = r'''        // Local product/version rows come from the selected installation, not the online list.
        private bool SelectLocalProducts(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
                return false;
            path = path.Trim().Trim('"');
            string buildInfo = Path.Combine(path, ".build.info");
            if (!File.Exists(buildInfo))
            {
                MessageBox.Show("Cannot find local .build.info in:\n" + path,
                    "WoW Forever", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }
            try
            {
                var found = new List<TACTProduct>();
                using (var fs = File.OpenRead(buildInfo))
                {
                    var ver = VerBarConfig.ReadVerBarConfig(fs);
                    for (int i = 0; i < ver.Count; ++i)
                    {
                        if (!ver[i].TryGetValue("Product", out string product) ||
                            !product.StartsWith("wow", StringComparison.OrdinalIgnoreCase))
                            continue;
                        if (found.Exists(x => x.Id.Equals(product, StringComparison.OrdinalIgnoreCase)))
                            continue;
                        ver[i].TryGetValue("Version", out string version);
                        bool forever = product.Equals("wow_classic_beta", StringComparison.OrdinalIgnoreCase) &&
                                       (version ?? "").StartsWith("1.60.", StringComparison.OrdinalIgnoreCase);
                        found.Add(new TACTProduct {
                            Id = product,
                            Name = (forever ? "WoW Forever" : product) + " - LOCAL " + (version ?? "?")
                        });
                    }
                }
                if (found.Count == 0)
                    throw new InvalidDataException("No WoW product was found in .build.info.");
                products.Clear();
                products.AddRange(found);
                productComboBox.DataSource = null;
                productComboBox.DataSource = products;
                int idx = products.FindIndex(x => x.Name.StartsWith("WoW Forever"));
                productComboBox.SelectedIndex = idx >= 0 ? idx : 0;
                detectedLocalPath = path;
                return true;
            }
            catch (Exception ex)
            {
                MessageBox.Show("Unable to read local installation:\n" + ex.Message,
                    "WoW Forever", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

'''
patch("CASCExplorer/OpenStorageForm.cs", [
    ("using System.Configuration;\nusing System.Windows.Forms;",
     "using System.Configuration;\nusing System.IO;\nusing System.Windows.Forms;"),
    ("        private List<TACTProduct> products = new List<TACTProduct>();",
     "        private List<TACTProduct> products = new List<TACTProduct>();\n" +
     "        private string detectedLocalPath;"),
    ("        private void Button1_Click(object sender, EventArgs e)", reader + "        private void Button1_Click(object sender, EventArgs e)"),
    ("            textBox1.Text = path;",
     "            textBox1.Text = path;\n            SelectLocalProducts(path);"),
    ("        private void Button2_Click(object sender, EventArgs e)\n        {\n            if (productComboBox.SelectedIndex == -1)",
     "        private void Button2_Click(object sender, EventArgs e)\n        {\n" +
     "            string path = textBox1.Text.Trim().Trim('\"');\n" +
     "            if (!string.Equals(detectedLocalPath, path, StringComparison.OrdinalIgnoreCase)\n" +
     "                && !SelectLocalProducts(path))\n                return;\n\n" +
     "            if (productComboBox.SelectedIndex == -1)"),
    ("CASCConfig.LoadLocalStorageConfig(textBox1.Text, selectedProduct.Id);",
     "CASCConfig.LoadLocalStorageConfig(path, selectedProduct.Id);"),
    ("            StoragePath = textBox1.Text;", "            StoragePath = path;")
])

patch("CASCExplorer/InitForm.cs", [
    ("            CASCConfig.LoadFlags |= LoadFlags.FileIndex;",
     "            CASCConfig.LoadFlags |= LoadFlags.FileIndex;\n" +
     "            CASCConfig.UseOnlineFallbackForMissingFiles = online;"),
    ("                casc.Root.LoadListFile(Settings.Default.ListFilePath, backgroundWorker1);",
     """                // Custom overrides are loaded first and keep priority for their FileDataIDs.
                if (!online)
                {
                    string customListfile = Path.Combine(path, "forever-listfile.csv");
                    if (File.Exists(customListfile))
                        casc.Root.LoadListFile(customListfile, backgroundWorker1);

                    string localListfile = Path.Combine(path, "listfile.csv");
                    if (File.Exists(localListfile))
                        casc.Root.LoadListFile(localListfile, backgroundWorker1);
                }

                string defaultListfile = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "listfile.csv");
                string savedListfile = Settings.Default.ListFilePath;
                string selectedListfile = null;

                if (!string.IsNullOrWhiteSpace(savedListfile))
                {
                    string candidate = Path.IsPathRooted(savedListfile)
                        ? savedListfile
                        : Path.Combine(AppDomain.CurrentDomain.BaseDirectory, savedListfile);
                    if (File.Exists(candidate))
                        selectedListfile = candidate;
                }

                if (selectedListfile == null && File.Exists(defaultListfile))
                    selectedListfile = defaultListfile;

                if (selectedListfile != null)
                    casc.Root.LoadListFile(selectedListfile, backgroundWorker1);

                // Always supplement selected names with the bundled community listfile.
                if (File.Exists(defaultListfile) &&
                    !string.Equals(defaultListfile, selectedListfile, StringComparison.OrdinalIgnoreCase))
                    casc.Root.LoadListFile(defaultListfile, backgroundWorker1);

                if (selectedListfile == null && !File.Exists(defaultListfile))
                    Logger.WriteLine("No listfile found. Place listfile.csv beside CASCExplorer.exe.");"""),
    ("            using (var s = casc.OpenFile(\"DBFilesClient\\\\FileDataComplete.db2\"))\n            {\n                WDC1Reader fd = new WDC1Reader(s);",
     r'''            using (var s = casc.OpenFile("DBFilesClient\\FileDataComplete.db2"))
            {
                byte[] magic = new byte[4];
                if (s.Read(magic, 0, 4) != 4)
                    return;
                s.Position = 0;
                if (System.Text.Encoding.ASCII.GetString(magic) != "WDC1")
                {
                    Logger.WriteLine("FileDataComplete is not WDC1; use listfile for names.");
                    return;
                }
                WDC1Reader fd = new WDC1Reader(s);''')
])

patch("CASCExplorer/MainForm.cs", [
    ('            statusProgress.Visible = false;\n            statusLabel.Text = string.Format("Loaded {0} files ({1} names missing)", casc.Root.CountSelect - casc.Root.CountUnknown, casc.Root.CountUnknown);',
     '            statusProgress.Visible = false;\n            statusLabel.Text = string.Format("Loaded {0} files ({1} names missing)", casc.Root.CountSelect - casc.Root.CountUnknown, casc.Root.CountUnknown);\n' +
     '            Text = "CASCExplorer - " + cfg.Product + " - " + cfg.VersionName + " - " + (cfg.OnlineMode ? "ONLINE" : cfg.BasePath);')
])

patch("CASCExplorer/MainForm.cs", [
    ("                    viewHelper?.CASC?.Root?.LoadListFile(Settings.Default.ListFilePath);",
     """                    if (viewHelper?.CASC?.Root != null)
                    {
                        // Rebuild folders now; LoadListFile alone only updates the name dictionary.
                        viewHelper.CASC.Root.LoadListFile(Settings.Default.ListFilePath);
                        viewHelper.ChangeLocale(Settings.Default.LocaleFlags.ToString());
                    }""")
])

patch("CascLib/CascLib/CASCHandlerBase.cs", [
    ('''            catch// (Exception exc) when (!(exc is BLTEDecoderException))
            {
                return OpenFileOnline(tempEKey);
            }''',
     '''            catch// (Exception exc) when (!(exc is BLTEDecoderException))
            {
                if (Config.OnlineMode || CASCConfig.UseOnlineFallbackForMissingFiles)
                    return OpenFileOnline(tempEKey);
                throw;
            }'''),
    ('''            catch
            {
                if (Config.OnlineMode || CASCConfig.UseOnlineFallbackForMissingFiles)
                    ExtractFileOnline(tempEKey, path, name);
            }''',
     '''            catch
            {
                if (Config.OnlineMode || CASCConfig.UseOnlineFallbackForMissingFiles)
                    ExtractFileOnline(tempEKey, path, name);
                else
                    throw;
            }''')
])
# Community listfile contains millions of entries from other client builds.
# Avoid one log line per unknown/duplicate name and millions of progress callbacks.
patch("CascLib/CascLib/RootHandlers/WowRootHandler.cs", [
    ('                    string line;\n\n                    char[] splitChar',
     '                    string line;\n                    int lineNumber = 0;\n\n                    char[] splitChar'),
    ('                    while ((line = sr.ReadLine()) != null)\n                    {\n                        string[] tokens = line.Split(splitChar, 2);',
     '                    while ((line = sr.ReadLine()) != null)\n                    {\n                        lineNumber++;\n                        string[] tokens = line.Split(splitChar, 2);'),
    ('                            Logger.WriteLine($"Invalid fileDataId in listfile: {line}");\n                            continue;',
     '                            // A combined community listfile includes IDs missing in this game build.\n                            continue;'),
    ('                        else\n                            Logger.WriteLine($"Duplicate fileDataId {fileDataId} detected: {line}");\n\n                        worker?.ReportProgress((int)(sr.BaseStream.Position / (float)sr.BaseStream.Length * 100));',
     '                        // Existing names come from higher-priority custom listfiles.\n\n                        if (lineNumber % 25000 == 0)\n                            worker?.ReportProgress((int)(sr.BaseStream.Position / (float)sr.BaseStream.Length * 100));')
])


# Wire the new model/texture ID explorer into the CASCExplorer UI and .csproj.
patch("CASCExplorer/CASCExplorer.csproj", [
    ('    <Compile Include="FileScanner.cs" />',
     '    <Compile Include="FileScanner.cs" />\n    <Compile Include="ForeverDataBrowserForm.cs" />')
])
patch("CASCExplorer/MainForm.cs", [
    ('            useLVToolStripMenuItem.Checked = Settings.Default.OverrideArchive;',
     """            var foreverDbMenu = new ToolStripMenuItem("Forever DB2 browser (actual FileDataIDs)...");
            foreverDbMenu.Click += (src, args) =>
            {
                if (viewHelper.CASC == null)
                {
                    MessageBox.Show("Open the local WoW Forever CASC first.");
                    return;
                }
                try
                {
                    using (var browser = new ForeverDataBrowserForm(viewHelper.CASC))
                        browser.ShowDialog(this);
                }
                catch (Exception exc)
                {
                    MessageBox.Show("Cannot open Forever DB2 exports:\r\n" + exc.Message +
                        "\r\nKeep all six CSV files inside ForeverData beside the EXE.",
                        "WoW Forever DB2 Browser", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            };
            toolsToolStripMenuItem.DropDownItems.Add(foreverDbMenu);

            useLVToolStripMenuItem.Checked = Settings.Default.OverrideArchive;""')
])

print("Patch applied successfully.")
