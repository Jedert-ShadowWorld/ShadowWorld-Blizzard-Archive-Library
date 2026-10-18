using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows.Forms;
using CASCLib;

namespace CASCExplorer
{
    // Forever's own DB2 relationships, not guessed file names from a retail listfile.
    // Data files are provided by the user, alongside the portable EXE.
    public sealed class ForeverDataBrowserForm : Form
    {
        private readonly CASCHandler casc;
        private readonly Dictionary<string, Dictionary<string, string>> races;
        private readonly Dictionary<string, Dictionary<string, string>> chrModels;
        private readonly Dictionary<string, Dictionary<string, string>> displays;
        private readonly Dictionary<string, Dictionary<string, string>> creatureModels;
        private readonly List<Dictionary<string, string>> raceLinks;
        private readonly List<RaceRow> raceRows = new List<RaceRow>();
        private readonly List<ModelRow> modelRows = new List<ModelRow>();
        private readonly List<TextureRow> allTextures = new List<TextureRow>();
        private List<TextureRow> shownTextures;
        private readonly ListView racesView = MakeList();
        private readonly ListView modelsView = MakeList();
        private readonly ListView texturesView = MakeList();
        private readonly TextBox racesSearch = new TextBox();
        private readonly TextBox modelsSearch = new TextBox();
        private readonly TextBox texturesSearch = new TextBox();
        private readonly Label detail = new Label();
        private readonly Label status = new Label();
        private readonly Button exportRaceM2 = new Button { Text = "Export selected race M2" };
        private readonly Button exportRaceSkel = new Button { Text = "Export skeleton (.skel)" };
        private readonly Button exportRaceTextures = new Button { Text = "Export linked textures" };
        private readonly Button exportModel = new Button { Text = "Export selected M2" };
        private readonly Button exportTexture = new Button { Text = "Export selected BLP" };

        private sealed class RaceRow
        {
            public int RaceId, ChrModelId, DisplayId, ModelDataId, ModelFid, SkeletonFid;
            public string Name, Category, Sex;
            public List<int> Textures = new List<int>();
            public string SearchText => (Name + " " + Category + " " + Sex + " " + RaceId + " " + DisplayId + " " + ModelFid).ToLowerInvariant();
        }
        private sealed class ModelRow
        {
            public int ModelId, Fid;
            public string SearchText => (ModelId + " " + Fid).ToLowerInvariant();
        }
        private sealed class TextureRow
        {
            public int Fid, Usage, Material;
            public string SearchText => (Fid + " " + Material).ToLowerInvariant();
        }

        private static ListView MakeList()
        {
            return new ListView {
                View = View.Details, FullRowSelect = true, GridLines = true,
                HideSelection = false, Dock = DockStyle.Fill
            };
        }
        private static string Value(Dictionary<string, string> row, string key)
        {
            if (row == null) return "";
            return row.TryGetValue(key, out var result) ? result : "";
        }
        private static int Number(string s)
        {
            return int.TryParse(s, out int i) ? i : 0;
        }
        private static int Number(Dictionary<string, string> row, string key) => Number(Value(row, key));
        private static List<string> Parse(string line)
        {
            var cells = new List<string>();
            var token = new StringBuilder();
            bool quoted = false;
            for (int i = 0; i < line.Length; ++i)
            {
                char c = line[i];
                if (c == '"')
                {
                    if (quoted && i + 1 < line.Length && line[i + 1] == '"') { token.Append('"'); ++i; }
                    else quoted = !quoted;
                }
                else if (c == ';' && !quoted)
                {
                    cells.Add(token.ToString());
                    token.Clear();
                }
                else token.Append(c);
            }
            cells.Add(token.ToString());
            return cells;
        }
        private static List<Dictionary<string, string>> ReadRows(string dir, string table)
        {
            string path = Path.Combine(dir, table + ".csv");
            if (!File.Exists(path))
                throw new FileNotFoundException("Required Forever DB2 export is missing: " + path);
            using (var sr = new StreamReader(path, Encoding.UTF8))
            {
                var header = Parse(sr.ReadLine() ?? "");
                var result = new List<Dictionary<string, string>>();
                string line;
                while ((line = sr.ReadLine()) != null)
                {
                    if (string.IsNullOrWhiteSpace(line)) continue;
                    var cells = Parse(line);
                    if (cells.Count != header.Count)
                        throw new InvalidDataException(table + ".csv: incorrect column count on row " + (result.Count + 2));
                    var dict = new Dictionary<string, string>(StringComparer.Ordinal);
                    for (int i = 0; i < header.Count; ++i) dict.Add(header[i].TrimStart('\uFEFF'), cells[i]);
                    result.Add(dict);
                }
                return result;
            }
        }
        private static Dictionary<string, Dictionary<string, string>> Index(List<Dictionary<string, string>> rows)
        {
            return rows.GroupBy(r => Value(r, "ID")).ToDictionary(g => g.Key, g => g.First());
        }
        private static Dictionary<string, string> Get(Dictionary<string, Dictionary<string, string>> index, string id)
        {
            return index.TryGetValue(id, out var row) ? row : null;
        }
        private static IEnumerable<int> Ids(string csv)
        {
            return (csv ?? "").Split(',').Select(Number).Where(x => x > 0).Distinct();
        }

        // Also add a clearly-marked, DB2-derived directory to the MAIN CASC tree.
        // These names are generated shortcuts to the genuine FileDataIDs, never claimed
        // to be original on-disk client paths or decrypted by name inference.
        public static void AddVerifiedRaceFolders(CASCFolder root, CASCHandler casc)
        {
            if (root == null || casc == null || !(casc.Root is WowRootHandler wowRoot)) return;
            string baseDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ForeverData");
            string[] required = { "ChrRaces", "ChrModel", "ChrRaceXChrModel", "CreatureDisplayInfo", "CreatureModelData" };
            if (required.Any(x => !File.Exists(Path.Combine(baseDir, x + ".csv")))) return;

            try
            {
                var namedRaces = Index(ReadRows(baseDir, "ChrRaces"));
                var chrModels = Index(ReadRows(baseDir, "ChrModel"));
                var raceLinks = ReadRows(baseDir, "ChrRaceXChrModel");
                var displays = Index(ReadRows(baseDir, "CreatureDisplayInfo"));
                var modelData = Index(ReadRows(baseDir, "CreatureModelData"));
                var verified = new CASCFolder("Forever DB2 (verified FileDataID links)");
                int added = 0;

                void Link(CASCFolder folder, int fid, string extension)
                {
                    if (fid <= 0 || !wowRoot.FileExist(fid)) return;
                    // The selected game's root determines the real CASC hash.
                    ulong hash = wowRoot.GetHashByFileDataId(fid);
                    string name = extension.ToUpperInvariant().TrimStart('.') + "_FileDataID_" + fid + extension;
                    if (!folder.Files.ContainsKey(name))
                    {
                        folder.Files[name] = new CASCFile(hash, "ForeverDB2\\" + name);
                        added++;
                    }
                }

                foreach (var relation in raceLinks)
                {
                    var race = Get(namedRaces, Value(relation, "ChrRacesID"));
                    var chr = Get(chrModels, Value(relation, "ChrModelID"));
                    var disp = Get(displays, Value(chr, "DisplayID"));
                    var mdl = Get(modelData, Value(disp, "ModelID"));
                    if (race == null || mdl == null) continue;
                    string raceId = Value(race, "ID");
                    string raceName = Safe(Value(race, "Name_lang"));
                    bool playable = Number(race, "PlayableRaceBit") >= 0 &&
                                    Value(race, "PlayableRaceBit") != "-1";
                    string groupName = playable ? "Playable races" : "NPC and other races";
                    if (!verified.Folders.TryGetValue(groupName, out CASCFolder group))
                        verified.Folders[groupName] = group = new CASCFolder(groupName);
                    string raceFolderName = raceName + "_RaceID_" + raceId;
                    if (!group.Folders.TryGetValue(raceFolderName, out CASCFolder byRace))
                        group.Folders[raceFolderName] = byRace = new CASCFolder(raceFolderName);
                    string sexName = Value(relation, "Sex") == "0" ? "Male" : "Female";
                    if (!byRace.Folders.TryGetValue(sexName, out CASCFolder bySex))
                        byRace.Folders[sexName] = bySex = new CASCFolder(sexName);
                    Link(bySex, Number(mdl, "FileDataID"), ".m2");
                    // Mark the Male/Female folder expandable; resolve M2 chunk references
                    // lazily only when the user opens it.
                    if (!bySex.Folders.ContainsKey("Related assets (expand race to scan)"))
                        bySex.Folders["Related assets (expand race to scan)"] =
                            new CASCFolder("Related assets (expand race to scan)");
                    Link(bySex, Number(chr, "SkeletonFileDataID"), ".skel");
                    foreach (int fid in Ids(Value(disp, "TextureVariationFileDataID")))
                        Link(bySex, fid, ".blp");
                    Link(bySex, Number(disp, "PortraitTextureFileDataID"), ".blp");
                }
                if (added > 0)
                {
                    root.Folders[verified.Name] = verified;
                    Logger.WriteLine("ForeverDB2: inserted " + added + " verified FileDataID shortcuts into CASC tree.");
                }
            }
            catch (Exception ex) { Logger.WriteLine("Forever DB2 tree: " + ex.Message); }
        }

        public ForeverDataBrowserForm(CASCHandler source)
        {
            casc = source ?? throw new ArgumentNullException(nameof(source));
            string dataDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ForeverData");

            races = Index(ReadRows(dataDir, "ChrRaces"));
            chrModels = Index(ReadRows(dataDir, "ChrModel"));
            raceLinks = ReadRows(dataDir, "ChrRaceXChrModel");
            displays = Index(ReadRows(dataDir, "CreatureDisplayInfo"));
            creatureModels = Index(ReadRows(dataDir, "CreatureModelData"));

            foreach (var rel in raceLinks)
            {
                var race = Get(races, Value(rel, "ChrRacesID"));
                var model = Get(chrModels, Value(rel, "ChrModelID"));
                var display = Get(displays, Value(model, "DisplayID"));
                var creature = Get(creatureModels, Value(display, "ModelID"));
                var linkedTextures = Ids(Value(display, "TextureVariationFileDataID")).ToList();
                int portraitFid = Number(display, "PortraitTextureFileDataID");
                if (portraitFid > 0 && !linkedTextures.Contains(portraitFid))
                    linkedTextures.Add(portraitFid);
                raceRows.Add(new RaceRow {
                    RaceId = Number(rel, "ChrRacesID"),
                    ChrModelId = Number(rel, "ChrModelID"),
                    DisplayId = Number(model, "DisplayID"),
                    ModelDataId = Number(display, "ModelID"),
                    ModelFid = Number(creature, "FileDataID"),
                    SkeletonFid = Number(model, "SkeletonFileDataID"),
                    Name = Value(race, "Name_lang"),
                    Category = Number(race, "PlayableRaceBit") >= 0 &&
                               Value(race, "PlayableRaceBit") != "-1" ? "Playable" : "NPC / other",
                    Sex = Value(rel, "Sex") == "0" ? "Male" : Value(rel, "Sex") == "1" ? "Female" : "Other",
                    Textures = linkedTextures
                });
            }
            raceRows.Sort((a,b) => a.RaceId != b.RaceId ? a.RaceId.CompareTo(b.RaceId) : string.Compare(a.Sex,b.Sex,StringComparison.Ordinal));

            foreach (var row in creatureModels.Values)
                modelRows.Add(new ModelRow { ModelId = Number(row, "ID"), Fid = Number(row, "FileDataID") });
            modelRows.Sort((a,b) => a.ModelId.CompareTo(b.ModelId));

            // TextureFileData links FileDataID to MaterialResourcesID. It does NOT
            // provide a direct mapping of every BLP to every character model.
            string texturesPath = Path.Combine(dataDir, "TextureFileData.csv");
            if (!File.Exists(texturesPath)) throw new FileNotFoundException(texturesPath);
            foreach (string line in File.ReadLines(texturesPath).Skip(1))
            {
                var parts = line.Split(';');
                if (parts.Length != 3) continue;
                int fid = Number(parts[0]);
                if (fid > 0) allTextures.Add(new TextureRow { Fid = fid, Usage = Number(parts[1]), Material = Number(parts[2]) });
            }
            shownTextures = allTextures;

            Text = "WoW Forever | Real DB2 race/model/FileDataID browser";
            Width = 1260;
            Height = 790;
            StartPosition = FormStartPosition.CenterParent;
            MinimumSize = new Size(960, 560);
            var intro = new Label {
                Dock = DockStyle.Top, Height = 42,
                Text = "FROM YOUR FOREVER DB2 EXPORTS: ChrRaces -> ChrRaceXChrModel -> ChrModel -> CreatureDisplayInfo -> CreatureModelData -> M2 FileDataID.\r\n" +
                       "These are real relationships, NOT guessed Retail file paths. File existence is checked against your opened CASC.",
                Padding = new Padding(9, 4, 4, 4)
            };
            var tabs = new TabControl { Dock = DockStyle.Fill };
            var raceTab = new TabPage("Forever races (" + races.Count + ")");
            var modelTab = new TabPage("Creature models (" + modelRows.Count + ")");
            var textureTab = new TabPage("Texture IDs (" + allTextures.Count + ")");
            tabs.TabPages.AddRange(new[] { raceTab, modelTab, textureTab });
            BuildRaceTab(raceTab);
            BuildModelTab(modelTab);
            BuildTextureTab(textureTab);
            status.Text = $"Loaded {races.Count} races, {raceRows.Count} race-model relations, {modelRows.Count} model records, {allTextures.Count} texture records.";
            status.Dock = DockStyle.Bottom; status.Height = 24;
            status.Padding = new Padding(8, 3, 3, 3);
            Controls.Add(tabs);
            Controls.Add(status);
            Controls.Add(intro);
            RefreshRaces(); RefreshModels(); RefreshTextures();
        }

        private static void Columns(ListView list, params (string name, int width)[] columns)
        {
            foreach (var col in columns) list.Columns.Add(col.name, col.width);
        }
        private static Panel SearchPanel(TextBox box, string label)
        {
            var panel = new Panel { Dock = DockStyle.Top, Height = 43 };
            panel.Controls.Add(new Label { Text = label, Left = 9, Top = 12, Width = 290 });
            box.Left = 300; box.Top = 9; box.Width = 420; box.Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right;
            panel.Controls.Add(box);
            return panel;
        }
        private static Panel Buttons(params Button[] buttons)
        {
            var panel = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 40, FlowDirection = FlowDirection.LeftToRight };
            foreach (var b in buttons) { b.AutoSize = true; b.Margin = new Padding(5); panel.Controls.Add(b); }
            return panel;
        }
        private void BuildRaceTab(TabPage tab)
        {
            Columns(racesView, ("Race", 230), ("Category", 95), ("Sex", 75), ("Race ID", 72),
                ("ChrModel", 85), ("DisplayID", 85), ("Model FID", 100), ("Skeleton FID", 110), ("In CASC?", 80), ("Linked BLP IDs", 185));
            detail.Dock = DockStyle.Bottom;
            detail.Height = 70;
            detail.Padding = new Padding(8, 6, 8, 6);
            detail.BorderStyle = BorderStyle.Fixed3D;
            detail.Text = "Select a race to see DB2 relationships and linked files.";
            tab.Controls.Add(racesView);
            tab.Controls.Add(detail);
            tab.Controls.Add(Buttons(exportRaceM2, exportRaceSkel, exportRaceTextures));
            tab.Controls.Add(SearchPanel(racesSearch, "Race / FileDataID / DisplayID:"));
            racesSearch.TextChanged += (s,e) => RefreshRaces();
            racesView.SelectedIndexChanged += (s,e) => DescribeRace();
            exportRaceM2.Click += (s,e) => { var x = SelectedRace(); if (x != null) Extract(x.ModelFid, x.Name + "_" + x.Sex + "_" + x.ModelFid + ".m2"); };
            exportRaceSkel.Click += (s,e) => { var x = SelectedRace(); if (x != null) Extract(x.SkeletonFid, x.Name + "_" + x.Sex + "_" + x.SkeletonFid + ".skel"); };
            exportRaceTextures.Click += (s,e) => {
                var x = SelectedRace(); if (x == null) return;
                if (x.Textures.Count == 0) {
                    MessageBox.Show("This CreatureDisplayInfo row has no direct texture/portrait FileDataIDs.\r\n" +
                        "TextureFileData alone does not associate every texture with a model.",
                        "No direct DB2 texture links");
                    return;
                }
                ExtractMany(x.Textures.Select(fid => new KeyValuePair<int,string>(fid, x.Name + "_" + fid + ".blp")));
            };
        }
        private void BuildModelTab(TabPage tab)
        {
            Columns(modelsView, ("CreatureModelData ID", 185), ("M2 FileDataID", 150), ("In CASC?", 100));
            tab.Controls.Add(modelsView);
            tab.Controls.Add(Buttons(exportModel));
            tab.Controls.Add(SearchPanel(modelsSearch, "CreatureModelData or FileDataID:"));
            modelsSearch.TextChanged += (s,e) => RefreshModels();
            exportModel.Click += (s,e) => { var x = SelectedModel(); if (x != null) Extract(x.Fid, "CreatureModel_" + x.ModelId + "_FileData_" + x.Fid + ".m2"); };
        }
        private void BuildTextureTab(TabPage tab)
        {
            Columns(texturesView, ("Texture FileDataID", 175), ("UsageType", 100), ("MaterialResourcesID", 185));
            texturesView.VirtualMode = true;
            texturesView.RetrieveVirtualItem += (s,e) => {
                if (e.ItemIndex >= 0 && e.ItemIndex < shownTextures.Count) {
                    var r = shownTextures[e.ItemIndex];
                    e.Item = new ListViewItem(new[] { r.Fid.ToString(), r.Usage.ToString(), r.Material.ToString() });
                } else e.Item = new ListViewItem("");
            };
            tab.Controls.Add(texturesView);
            tab.Controls.Add(Buttons(exportTexture));
            tab.Controls.Add(SearchPanel(texturesSearch, "Texture FileDataID / MaterialResourcesID:"));
            texturesSearch.TextChanged += (s,e) => RefreshTextures();
            exportTexture.Click += (s,e) => {
                if (texturesView.SelectedIndices.Count == 0) return;
                int idx = texturesView.SelectedIndices[0];
                if (idx < 0 || idx >= shownTextures.Count) return;
                int fid = shownTextures[idx].Fid;
                Extract(fid, "Texture_FileData_" + fid + ".blp");
            };
        }
        private void RefreshRaces()
        {
            string find = racesSearch.Text.Trim().ToLowerInvariant();
            racesView.BeginUpdate();
            racesView.Items.Clear();
            foreach (var row in raceRows)
            {
                if (find.Length > 0 && !row.SearchText.Contains(find)) continue;
                var item = new ListViewItem(new[] {
                    row.Name, row.Category, row.Sex, row.RaceId.ToString(), row.ChrModelId.ToString(),
                    row.DisplayId.ToString(), row.ModelFid.ToString(), row.SkeletonFid.ToString(),
                    (row.ModelFid > 0 && casc.FileExists(row.ModelFid)) ? "YES" : "NO",
                    string.Join(", ", row.Textures)
                });
                item.Tag = row;
                racesView.Items.Add(item);
            }
            racesView.EndUpdate();
        }
        private void RefreshModels()
        {
            string find = modelsSearch.Text.Trim().ToLowerInvariant();
            modelsView.BeginUpdate();
            modelsView.Items.Clear();
            foreach (var row in modelRows)
            {
                if (find.Length > 0 && !row.SearchText.Contains(find)) continue;
                var item = new ListViewItem(new[] { row.ModelId.ToString(), row.Fid.ToString(),
                    row.Fid > 0 && casc.FileExists(row.Fid) ? "YES" : "NO" });
                item.Tag = row;
                modelsView.Items.Add(item);
            }
            modelsView.EndUpdate();
        }
        private void RefreshTextures()
        {
            string find = texturesSearch.Text.Trim().ToLowerInvariant();
            shownTextures = find.Length == 0 ? allTextures :
                allTextures.Where(r => r.SearchText.Contains(find)).ToList();
            texturesView.VirtualListSize = 0;
            texturesView.VirtualListSize = shownTextures.Count;
        }
        private RaceRow SelectedRace() => racesView.SelectedItems.Count > 0 ? racesView.SelectedItems[0].Tag as RaceRow : null;
        private ModelRow SelectedModel() => modelsView.SelectedItems.Count > 0 ? modelsView.SelectedItems[0].Tag as ModelRow : null;
        private void DescribeRace()
        {
            var x = SelectedRace();
            if (x == null) { detail.Text = "Select a race."; return; }
            detail.Text = $"{x.Name} [{x.Category}; {x.Sex}], ChrRaces={x.RaceId}, ChrModel={x.ChrModelId}, CreatureDisplayInfo={x.DisplayId}, CreatureModelData={x.ModelDataId}\r\n" +
                $"REAL M2 FileDataID={x.ModelFid} (in CASC: {casc.FileExists(x.ModelFid)}), skeleton={x.SkeletonFid}, directly linked BLP IDs=[{string.Join(", ", x.Textures)}].\r\n" +
                "Full character skins require additional DB2/M2 dependencies; this does not invent missing paths.";
        }
        private static string Safe(string text)
        {
            return new string((text ?? "").Select(c => char.IsLetterOrDigit(c) || c == '-' || c == '_' ? c : '_').ToArray());
        }
        private void Extract(int id, string label) => ExtractMany(new[] { new KeyValuePair<int,string>(id, label) });
        private void ExtractMany(IEnumerable<KeyValuePair<int,string>> files)
        {
            var list = files.ToList();
            using (var dlg = new FolderBrowserDialog { Description = "Choose a folder to export real Forever CASC files" })
            {
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                int ok = 0;
                var errors = new List<string>();
                foreach (var pair in list)
                {
                    int fid = pair.Key;
                    if (fid <= 0 || !casc.FileExists(fid)) { errors.Add("FileDataID " + fid + " is not present in the opened CASC."); continue; }
                    try
                    {
                        string file = Safe(Path.GetFileNameWithoutExtension(pair.Value)) + Path.GetExtension(pair.Value).ToLowerInvariant();
                        // This is an EXPORT name, never a claim about an original client path.
                        casc.SaveFileTo(fid, file, dlg.SelectedPath);
                        ++ok;
                    }
                    catch (Exception ex) { errors.Add(fid + ": " + ex.Message); }
                }
                string msg = "Exported " + ok + "/" + list.Count + " selected CASC files.";
                if (errors.Count > 0) msg += "\r\n" + string.Join("\r\n", errors.Take(7));
                MessageBox.Show(this, msg, "WoW Forever DB2 export", MessageBoxButtons.OK,
                    errors.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
            }
        }
    }
}
