#!/usr/bin/env node
// The CV map, end to end: remapped over HostLink (as the web control page
// does it), then heard in the emulator.
//
//   node emu-tests-hostlink/cvmap.mjs <alchemy-lab emu dir> <tally emu exe>
//
// Needs the emu-hostlink emulator (its web/hostlink.js and --hostlink).
// Over HostLink: J6 -> Freq (+1), J5 -> Shift (+1), J4 -> Off. Then the
// emulator's own script listens: +1 V on J6 is C5, 0.75 V on J5 moves the
// tone onto its 2nd harmonic (and no longer triggers anything), 5 V on J4
// changes nothing.
import { spawn } from "node:child_process";
import { createConnection } from "node:net";
import { mkdtempSync, writeFileSync, existsSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [emuDir, exe] = process.argv.slice(2);
if (!emuDir || !exe) { console.error("usage: cvmap.mjs <alchemy-lab emu dir> <tally emu exe>"); process.exit(2); }
const { HostLink, encodeState } = await import(pathToFileURL(join(emuDir, "web/hostlink.js")).href);

// Sections and knob slots (src/tally/cv_map.h): OSC P2 = Freq, OSC P3 = Shift.
const SEC = { off: 0, osc: 1 };

const dir = mkdtempSync(join(tmpdir(), "tally-cv-"));
const sockPath = join(dir, "hl.sock"), script = join(dir, "listen.emu");
writeFileSync(script, [
  "pot 1 0", "pot 2 0.5", "pot 3 0", "pot 4 0", "pot 5 0", "pot 6 0",
  "wait 6000",
  "expect tone 261.6 > 0.2      # Centre: C4",
  "cv 6 1",
  "wait 600",
  "expect tone 523.3 > 0.2      # J6 -> Freq: +1 V = C5",
  "expect tone 261.6 < 0.02",
  "cv 6 0",
  "wait 600",
  "expect tone 261.6 > 0.2",
  "cv 5 0.75",
  "wait 600",
  "expect tone 523.3 > 0.2      # J5 -> Shift: 0.15 = harmonic 2",
  "expect tone 261.6 < 0.05",
  "cv 5 0",
  "wait 600",
  "cv 4 5",
  "wait 600",
  "expect tone 261.6 > 0.2      # J4 -> Off: nothing moves",
  "expect tone 523.3 < 0.01",
  "expect alive",
  "",
].join("\n"));

const emu = spawn(exe, ["--headless", "--script", script, "--flash", "none", "--hostlink", sockPath],
  { stdio: ["ignore", "pipe", "inherit"] });
let out = "";
emu.stdout.on("data", (b) => { out += b; });
const done = new Promise((res) => emu.on("exit", (code) => res(code)));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

try {
  let sock = null;
  for (let i = 0; i < 100 && !sock; i++) {
    if (existsSync(sockPath)) sock = await new Promise((res) => {
      const s = createConnection(sockPath); s.once("connect", () => res(s)); s.once("error", () => res(null));
    });
    if (!sock) await sleep(100);
  }
  if (!sock) throw new Error("no HostLink socket");
  const link = new HostLink({ write: (u8) => new Promise((r) => sock.write(u8, r)),
                              onData: (cb) => sock.on("data", (b) => cb(new Uint8Array(b))) });
  let id = null;
  for (let i = 0; i < 25 && !id; i++) id = await link.hello().catch(() => null);
  const desc = await link.descriptor();
  const live = encodeState(desc, await link.getLive(), {
    cv_j4_section: SEC.off, cv_j4_knob: 0, cv_j4_amount: 1,
    cv_j5_section: SEC.osc, cv_j5_knob: 2, cv_j5_amount: 1,
    cv_j6_section: SEC.osc, cv_j6_knob: 1, cv_j6_amount: 1,
  });
  await link.setLive(live);
  console.log("mapped J4 -> Off, J5 -> Shift, J6 -> Freq over HostLink");
  sock.destroy();
} catch (e) {
  console.log(`FAIL  setup: ${e.message}`);
  emu.kill("SIGKILL");
}
const code = await done;
rmSync(dir, { recursive: true, force: true });
process.stdout.write(out.split("\n").filter((l) => /PASS|FAIL/.test(l)).join("\n") + "\n");
const ok = code === 0 && !/FAIL/.test(out);
console.log(ok ? "all passed" : "FAILED");
process.exit(ok ? 0 : 1);
