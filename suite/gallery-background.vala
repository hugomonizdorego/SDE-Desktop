// SPDX-License-Identifier: GPL-2.0-or-later
// SDE integration copyright (C) 2026 Hugo Moniz do Rego.
public class Gth.DesktopBackground {
	public async void set_file (Gtk.Window window, Gth.FileData file_data, Cancellable cancellable) throws Error {
		var desktop = Environment.get_variable ("XDG_CURRENT_DESKTOP");
		if (desktop != null && "SDE" in desktop.split (":")) {
			var path = file_data.file.get_path ();
			if (path == null) throw new IOError.NOT_SUPPORTED ("SDE wallpaper requires a local image");
			var process = new Subprocess.newv ({ "sde-settings", "--wallpaper", path }, SubprocessFlags.NONE);
			yield process.wait_check_async (cancellable);
			return;
		}
		var portal = new Xdp.Portal ();
		var parent = Xdp.parent_new_gtk (window);
		yield portal.set_wallpaper (parent, file_data.file.get_uri (),
			Xdp.WallpaperFlags.BACKGROUND | Xdp.WallpaperFlags.LOCKSCREEN, cancellable);
	}
}
