# Simulator artwork

Sample crests, player cutouts and news photos for the simulator's `MockImageProvider`.
They are third-party images, so they are git-ignored; fetch them with `node sim/assets/fetch.mjs` from the repo root (needs `npm install` in `helper/` for `sharp`).
Files are downsized to at most 96x96 (crests, cutouts) or 240x135 (news photos), and skipped if already present.
File names follow `mock_asset_name()` in `sim/mock_image_provider.cpp`: `crest-<id>.png`, `espn-<id>.png`, `cutout-<Name>.png`, `img-<basename>`.
