# Poliigon Bridge for Unreal Engine

Unofficial in-editor Poliigon browser for Unreal Engine 5.4+. Search the library, click **Get**,
and the asset lands in `/Content/Poliigon` ready to use — materials as instances of a generated
Substrate master material, models as static meshes with materials auto-assigned.

Sign-in uses the normal Poliigon website login with your own account. The API layer is a native
port of the client inside Poliigon's GPL-licensed Blender addon.

## Install

1. Copy the `PoliigonBridge` folder into `YourProject/Plugins/`.
2. Open the project and accept the module rebuild prompt (requires Visual Studio 2022 with the
   C++ game development workload).
3. Enable Substrate: **Project Settings → Rendering → Substrate**, restart.
4. Open **Tools → Poliigon Bridge**, click **Sign in with Poliigon**.

Settings live under **Project Settings → Plugins → Poliigon Bridge**.

## Notes

- Free assets are labeled FREE; the "Free only" checkbox filters to them. Paid assets use
  credits from your plan, exactly like downloading from the website.
- Every material instance exposes tint, roughness remap, normal strength, UV tiling/offset/rotation,
  world-aligned projection, macro anti-tiling variation, AO/emission controls and displacement
  (enable Tessellation on `M_PoliigonMaster` for Nanite displacement).
- HDRIs and brushes are not supported yet.
- Use at human pace — Poliigon enforces fair-use limits server-side. This is a bridge, not a scraper.

## License

GPL-2.0-or-later (see LICENSE). Contains logic derived from the Poliigon Blender addon (GPL).
Not affiliated with Poliigon.
