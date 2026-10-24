# Deskside FC demo site

A static showcase for the project: an interactive demo of the five UI screens, the real simulator screenshots, architecture diagrams, measured numbers and a build overview.

Plain HTML, CSS and ES modules. No build step, no framework, no analytics. The only external request is the Google Fonts stylesheet (Big Shoulders Display and Hanken Grotesk, both SIL OFL).

## Run locally

```sh
python3 -m http.server 8080 --directory site
# open http://localhost:8080
```

ES modules need an HTTP server; opening `index.html` from disk will not run the demo.

## Deploy

```sh
vercel deploy site          # preview
vercel deploy site --prod   # production
```

`vercel.json` turns on clean URLs, sets security headers (including a Content-Security-Policy that allows only this origin plus Google Fonts) and a one-hour cache for `/img`, `/css` and `/js`. File names are not hashed, so there is no `immutable` cache.

## Layout

- `index.html` page content, diagrams (inline SVG), meta tags and favicon (inline SVG).
- `css/style.css` page styles. `css/device.css` the 240x320 demo screen.
- `js/config.js` holds the repository URL in one constant (`REPO_URL`); `js/main.js` fills every `data-repo-path` link from it.
- `js/demo.js`, `js/teams.js`, `js/sound.js` the interactive device, team colours and the optional synthesized air-horn.
- `img/` screenshots copied from `docs/images/screens/` (simulator renders) and `og.png`.

## Notes

- The demo uses invented players and generic drawn shields. No crests, logos or photos are used.
- The two photographs of the physical device are deliberately not used; they will be replaced later.
- Sound is off by default and only plays after you turn it on and score.
