// Read-only Settings probe. Uses the same selection as BrowserService; never
// launches a browser or downloads one while the user opens Settings.
import { chromium } from "playwright-core";
import { probeEngine } from "./engine.mjs";

const result = await probeEngine({ playwrightExecutable: chromium.executablePath() });
process.stdout.write(JSON.stringify(result));
process.exitCode = result.available ? 0 : 1;
