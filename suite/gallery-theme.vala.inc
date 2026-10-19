		// SDE appearance, respecting the system theme when explicitly selected.
		var appearance = new KeyFile ();
		var config = Path.build_filename (Environment.get_user_config_dir (), "sde", "sde.conf");
		try {
			appearance.load_from_file (config, KeyFileFlags.NONE);
			bool follow = true;
			try { follow = appearance.get_string ("suite", "gtk_theme") != "system"; } catch (Error e) {}
			if (follow) {
				var dark = appearance.get_string ("appearance", "theme") == "dark";
				Adw.StyleManager.get_default ().color_scheme = dark ? Adw.ColorScheme.FORCE_DARK : Adw.ColorScheme.FORCE_LIGHT;
			}
		} catch (Error e) {}
