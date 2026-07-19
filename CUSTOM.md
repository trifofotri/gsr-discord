# Downstream Flatpak Bridge Change

This unofficial fork is based on GPU Screen Recorder 5.15.1 at upstream commit
`319097c1d24254dafd7c7c022e92001e594faf76`. Tony Lobanovskiy added the
downstream change described here in July 2026. It is distributed under
`GPL-3.0-only`, matching the upstream project.

## Change

When the recorder runs inside Flatpak, Wayland capture launches
`gsr-wayland-bridge` on the host. Upstream resolved that executable through a
fixed system-wide path. This fork reads the `app-path` recorded in the running
sandbox's `/.flatpak-info` and appends `/bin/gsr-wayland-bridge`.

The path therefore identifies the application tree mounted at `/app` for the
running sandbox and supports per-user, system, alternate-architecture, renamed,
and `--app-path` Flatpak launches without probing or shell execution.

The bridge is used only when `FLATPAK_ID` is present. If metadata or bridge
startup fails, the existing code falls back to the sandbox's Wayland
connection.

## Related Repositories

- [Customized GTK frontend](https://github.com/antonlobanovskiy/gpu-screen-recorder-gtk)
- [Flatpak packaging fork](https://github.com/antonlobanovskiy/com.dec05eba.gpu_screen_recorder)

Report problems introduced by this change in this GitHub repository rather than
to the upstream author.
