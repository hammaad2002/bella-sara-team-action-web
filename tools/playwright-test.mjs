import { chromium } from "playwright-core";

const browser = await chromium.launchPersistentContext("work/chrome-profile-5574", {
  headless: true,
  viewport: { width: 1280, height: 960 },
  executablePath: "/usr/bin/google-chrome",
  args: [
    "--autoplay-policy=no-user-gesture-required",
    "--enable-gpu",
    "--ignore-gpu-blocklist",
    "--use-angle=vulkan",
    "--enable-features=Vulkan",
    "--enable-unsafe-swiftshader",
  ],
});

const page = (browser.pages().length > 0) ? browser.pages()[0] : (await browser.newPage());

const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
page.on("console", (m) => {
  if (m.type() === "error") errors.push(m.text());
});

console.log("Navigating to http://localhost:5574/?autoplay&capture=250 ...");
await page.goto("http://localhost:5574/?autoplay&capture=250");

// Wait for running state
await page.waitForFunction(() => window.__bella?.state === "running", null, { timeout: 30000 });
console.log("Game state is running!");

// Wait for at least 10 presented frames
await page.waitForFunction(() => (window.__bella?.frames ?? 0) >= 10, null, { timeout: 30000 });
console.log("Frames presented:", await page.evaluate(() => window.__bella.frames));

// Now test mouse movement on the canvas
const canvas = page.locator("#canvas");
const box = await canvas.boundingBox();
console.log("Canvas box:", box);

if (box) {
  // Move mouse across canvas
  await page.mouse.move(box.x + 100, box.y + 100);
  await page.waitForTimeout(200);
  await page.mouse.move(box.x + 400, box.y + 300);
  await page.waitForTimeout(200);
  await page.mouse.move(box.x + 600, box.y + 500);
  await page.waitForTimeout(200);
  console.log("Moved mouse across canvas to (100,100), (400,300), (600,500)");
}

// Check console / engine logs
const logTail = await page.evaluate(() => (window.__bella?.log ?? []).slice(-20));
console.log("Engine log tail:\n" + logTail.join("\n"));

await page.screenshot({ path: "work/playwright-test.png" });
console.log("Saved screenshot to work/playwright-test.png");

await browser.close();
console.log("Playwright test completed successfully!");
