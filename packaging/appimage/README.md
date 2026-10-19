# appimage

an appimage is a self-contained bundle that must be (re)built on the target machine: there is no static recipe to edit here. everything needed is scripted:

```sh
sh ../scripts/build-appimage.sh   # builds from the repository source tree
sh ../scripts/build-appimage.sh /path/to/atlifm-1.0.0-fixed.tar.gz /path/to/dist
```

what the script does:

1. unpacks the source tree (or the archive you pass; the two fixes are
   already in the source);
2. builds with cmake + ninja;
3. assembles an AppDir (desktop file, 512px icon, metainfo, man page);
4. bundles Qt 6 with `linuxdeploy` + `linuxdeploy-plugin-qt`
   (xcb + wayland + offscreen platform plugins, image/SVG formats);
5. packs with `appimagetool`.

the helper tools are downloaded once into
`${XDG_CACHE_HOME:-$HOME/.cache}/atlifm-appimage`; FUSE is not required
(the tools run with `APPIMAGE_EXTRACT_AND_RUN=1`).

to use the bundle: `chmod +x atlifm-1.0.0-<arch>.AppImage` and run it; nothing is installed into the system. the CI workflow builds bundles for aarch64 and x86_64; attach them to a GitHub Release to distribute.
