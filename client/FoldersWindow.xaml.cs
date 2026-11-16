using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;

namespace Sleepy
{
    public partial class FoldersWindow : Window
    {
        readonly ConfigStore _cfg;
        readonly Library _lib;

        public FoldersWindow(ConfigStore cfg, Library lib)
        {
            _cfg = cfg; _lib = lib;
            InitializeComponent();
            Native.UseDarkTitleBar(this);
            Reload();
        }

        void Reload()
        {
            var folders = _cfg.Get().Folders;
            FolderList.ItemsSource = folders;
            EmptyText.Visibility = folders.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        void Scan() { Task.Run(() => _lib.Scan(_cfg.Get().Folders)); }

        void Add_Click(object sender, RoutedEventArgs e)
        {
            var path = FolderPicker.Pick(this, "Choose a folder with PS4 packages");
            if (string.IsNullOrEmpty(path) || !Directory.Exists(path)) return;
            _cfg.Update(c => { if (!c.Folders.Any(f => string.Equals(f, path, StringComparison.OrdinalIgnoreCase))) c.Folders.Add(path); });
            Reload();
            Scan();
        }

        void Remove_Click(object sender, RoutedEventArgs e)
        {
            var path = (string)((Button)sender).Tag;
            _cfg.Update(c => c.Folders.RemoveAll(f => string.Equals(f, path, StringComparison.OrdinalIgnoreCase)));
            Reload();
            Scan();
        }

        void Rescan_Click(object sender, RoutedEventArgs e) { Scan(); Close(); }
        void Done_Click(object sender, RoutedEventArgs e) { Close(); }
    }
}
