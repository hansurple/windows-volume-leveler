using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        try
        {
            bool firstInstance;
            using (var instance = new Mutex(true, @"Local\VolumeLevelerTray", out firstInstance))
            {
                if (!firstInstance)
                {
                    MessageBox.Show("面板已经打开。双击系统托盘里的音量均衡器图标即可返回。", "音量均衡器");
                    return;
                }
                try
                {
                    using (var window = new LevelerWindow(AppDomain.CurrentDomain.BaseDirectory))
                        Application.Run(window);
                }
                finally
                {
                    instance.ReleaseMutex();
                }
            }
        }
        catch (Exception error)
        {
            MessageBox.Show(error.Message, "音量均衡器无法启动", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}

internal sealed class LevelerSettings : IDisposable
{
    public const int EnabledBit = 128;
    public const int DefaultTarget = 24;
    public const int Minimum = 16;
    public const int Maximum = 32;

    private readonly MemoryMappedFile mapping;
    private readonly MemoryMappedViewAccessor view;

    public LevelerSettings(string path)
    {
        try
        {
            using (var initial = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
            {
                var bytes = BitConverter.GetBytes(DefaultTarget);
                initial.Write(bytes, 0, bytes.Length);
                initial.Flush(true);
            }
        }
        catch (IOException)
        {
            if (!File.Exists(path))
                throw;
        }

        using (var file = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite))
        {
            if (file.Length != sizeof(int))
                throw new IOException("设置文件损坏。关闭托盘和音频后，将 VolumeLeveler.settings 移到回收站，再打开工具重建设置。");
            mapping = MemoryMappedFile.CreateFromFile(file, null, 0, MemoryMappedFileAccess.ReadWrite,
                HandleInheritability.None, true);
        }
        try
        {
            view = mapping.CreateViewAccessor(0, sizeof(int), MemoryMappedFileAccess.ReadWrite);
        }
        catch
        {
            mapping.Dispose();
            throw;
        }
    }

    public int Read()
    {
        var value = view.ReadInt32(0);
        var target = value & 127;
        return (value & ~255) == 0 && target >= Minimum && target <= Maximum
            ? value
            : DefaultTarget;
    }

    public unsafe void Save(int target, bool enabled)
    {
        byte* address = null;
        view.SafeMemoryMappedViewHandle.AcquirePointer(ref address);
        try
        {
            var value = target | (enabled ? EnabledBit : 0);
            Interlocked.Exchange(ref *(int*)(address + view.PointerOffset), value);
        }
        finally
        {
            view.SafeMemoryMappedViewHandle.ReleasePointer();
        }
        view.Flush();
    }

    public void Dispose()
    {
        view.Dispose();
        mapping.Dispose();
    }
}

internal sealed class LevelerWindow : Form
{
    private readonly LevelerSettings settings;
    private readonly NotifyIcon tray;
    private readonly ToolStripMenuItem trayToggle;
    private readonly TrackBar target;
    private readonly CheckBox enabled;
    private readonly Label targetValue;
    private readonly Label status;
    private readonly string pluginPath;
    private bool exiting;

    public LevelerWindow(string folder)
    {
        pluginPath = Path.Combine(folder, "VolumeLeveler.dll");
        if (!File.Exists(pluginPath))
            throw new FileNotFoundException("请将 VolumeLeveler.dll 和托盘工具放在同一目录。", pluginPath);
        settings = new LevelerSettings(Path.Combine(folder, "VolumeLeveler.settings"));
        var saved = settings.Read();

        Text = "音量均衡器";
        Icon = SystemIcons.Information;
        Font = new Font("Microsoft YaHei UI", 9F);
        StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.Font;
        AutoScaleDimensions = new SizeF(7F, 17F);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        ClientSize = new Size(430, 281);
        BackColor = Color.FromArgb(248, 249, 251);
        ForeColor = Color.FromArgb(32, 41, 56);

        var header = new Label {
            Text = "音量均衡器", Font = new Font(Font.FontFamily, 16F, FontStyle.Bold),
            Location = new Point(20, 16), AutoSize = true
        };
        enabled = new CheckBox {
            Text = "启用自动均衡", AutoSize = true, Location = new Point(24, 62),
            Checked = (saved & LevelerSettings.EnabledBit) != 0
        };
        var targetLabel = new Label {
            Text = "目标音量", AutoSize = true, Location = new Point(21, 101)
        };
        targetValue = new Label {
            Location = new Point(308, 100), Size = new Size(100, 20),
            TextAlign = ContentAlignment.MiddleRight, ForeColor = Color.FromArgb(23, 76, 151)
        };
        target = new TrackBar {
            Name = "Target", AccessibleName = "目标音量",
            Location = new Point(18, 124), Size = new Size(394, 38),
            Minimum = -LevelerSettings.Maximum, Maximum = -LevelerSettings.Minimum,
            Value = -(saved & 127), TickFrequency = 4, LargeChange = 4, SmallChange = 1
        };
        var quieter = new Label {
            Text = "较轻", Location = new Point(21, 160), AutoSize = true
        };
        var louder = new Label {
            Text = "较响", Location = new Point(359, 160), Size = new Size(48, 18),
            TextAlign = ContentAlignment.MiddleRight
        };
        status = new Label {
            Location = new Point(21, 189), Size = new Size(390, 40),
            ForeColor = Color.FromArgb(72, 81, 95)
        };
        var configure = new Button {
            Text = "配置…", Location = new Point(21, 238), Size = new Size(112, 31)
        };
        configure.Click += delegate { ShowSetup(); };
        var hide = new Button {
            Text = "收起到托盘", Location = new Point(297, 238), Size = new Size(112, 31)
        };
        hide.Click += delegate { Hide(); };
        Controls.AddRange(new Control[] {
            header, enabled, targetLabel, targetValue, target, quieter, louder, status, configure, hide
        });

        var menu = new ContextMenuStrip();
        menu.Items.Add("打开面板", null, delegate { ShowPanel(); });
        trayToggle = new ToolStripMenuItem("启用自动均衡");
        trayToggle.Click += delegate { enabled.Checked = !enabled.Checked; };
        menu.Items.Add(trayToggle);
        menu.Items.Add("退出面板", null, delegate { exiting = true; Close(); });
        tray = new NotifyIcon { Icon = Icon, ContextMenuStrip = menu, Visible = true };
        tray.DoubleClick += delegate { ShowPanel(); };
        target.ValueChanged += delegate { SaveSettings(); };
        enabled.CheckedChanged += delegate { SaveSettings(); };
        FormClosing += delegate(object sender, FormClosingEventArgs e)
        {
            if (!exiting && e.CloseReason == CloseReason.UserClosing)
            {
                e.Cancel = true;
                Hide();
            }
        };
        UpdateLabels(null);
    }

    private void ShowPanel()
    {
        Show();
        WindowState = FormWindowState.Normal;
        Activate();
    }

    private void SaveSettings()
    {
        try
        {
            settings.Save(-target.Value, enabled.Checked);
            UpdateLabels(null);
        }
        catch (Exception error)
        {
            UpdateLabels("保存设置失败。 " + error.Message);
        }
    }

    private void UpdateLabels(string error)
    {
        var level = target.Value + " dBFS";
        targetValue.Text = level;
        status.Text = error ?? (enabled.Checked
            ? "已启用自动均衡。播放设备需在 Equalizer APO 中配置。"
            : "自动均衡已关闭。");
        trayToggle.Checked = enabled.Checked;
        tray.Text = error != null ? "音量均衡器 · 保存失败"
            : enabled.Checked ? "音量均衡器 · 目标 " + level : "音量均衡器 · 已关闭";
    }

    private void ShowSetup()
    {
        var block = "# Volume Leveler v2\r\nDevice: all\r\nStage: post-mix\r\nChannel: all\r\n"
            + "VSTPlugin: Library \"" + pluginPath + "\"\r\n";
        using (var dialog = new Form())
        {
            dialog.Text = "配置 Equalizer APO";
            dialog.Font = Font;
            dialog.FormBorderStyle = FormBorderStyle.FixedDialog;
            dialog.StartPosition = FormStartPosition.CenterParent;
            dialog.MaximizeBox = dialog.MinimizeBox = false;
            dialog.ClientSize = new Size(542, 267);
            var instructions = new Label {
                Text = "在 Equalizer APO 的设备选择器中勾选耳机和扬声器。\r\n"
                    + "初次使用时，将以下配置追加到主 config.txt 末尾。\r\n"
                    + "从 v1 升级时，用它替换旧的 Peak Limiter 配置块。保存后启用自动均衡。",
                Location = new Point(14, 14), Size = new Size(517, 68)
            };
            var content = new TextBox {
                Location = new Point(14, 87), Size = new Size(514, 117), Text = block,
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false
            };
            var copy = new Button { Text = "复制配置", Location = new Point(14, 222), Size = new Size(100, 29) };
            copy.Click += delegate
            {
                try { Clipboard.SetText(block); }
                catch (Exception error) { MessageBox.Show(dialog, error.Message, "复制失败"); }
            };
            var open = new Button { Text = "打开配置文件", Location = new Point(123, 222), Size = new Size(123, 29) };
            open.Click += delegate
            {
                try
                {
                    var install = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "EqualizerAPO");
                    var configured = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\EqualizerAPO", "ConfigPath", null) as string;
                    var path = Path.Combine(String.IsNullOrEmpty(configured) ? Path.Combine(install, "config") : configured, "config.txt");
                    if (!File.Exists(path))
                        throw new FileNotFoundException("没有找到 Equalizer APO 配置文件。请先安装，或手动打开主配置文件。");
                    Process.Start("notepad.exe", "\"" + path + "\"");
                }
                catch (Exception error) { MessageBox.Show(dialog, error.Message, "打开配置文件"); }
            };
            var close = new Button {
                Text = "关闭", Location = new Point(428, 222), Size = new Size(100, 29), DialogResult = DialogResult.Cancel
            };
            dialog.CancelButton = close;
            dialog.Controls.AddRange(new Control[] { instructions, content, copy, open, close });
            dialog.ShowDialog(this);
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            if (tray != null)
            {
                tray.Visible = false;
                tray.ContextMenuStrip.Dispose();
                tray.Dispose();
            }
            if (settings != null)
                settings.Dispose();
        }
        base.Dispose(disposing);
    }
}
