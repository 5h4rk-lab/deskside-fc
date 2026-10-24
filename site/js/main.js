import { REPO_URL } from "./config.js";
import { initDemo } from "./demo.js";

document.querySelectorAll("[data-repo-path]").forEach((a) => {
  const p = a.getAttribute("data-repo-path");
  a.href = p ? `${REPO_URL}/${p}` : REPO_URL;
});
const mount = document.getElementById("device");
if (mount) initDemo(mount);
