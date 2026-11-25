import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { ConfirmGate, guardTools } from "../confirm.js";

const INV = ["Bandages", "Ear drops", "Ibuprofen", "Allergy tablets", "Thermometer", "Cough syrup", "Tweezers", "Gauze",
  "Antiseptic wipes", "Eye drops", "Screwdriver", "AA batteries", "Duct tape", "Scissors", ""];
const { cases } = JSON.parse(readFileSync(new URL("./cases.json", import.meta.url), "utf8"));

function rig(opts = {}) {
  let t = 0;
  const moves = [], blocked = [];
  const gate = new ConfirmGate(INV, { now: () => t });
  const tools = guardTools({
    gate, inventory: INV, graceMs: 300,
    sleep: async ms => { t += ms; opts.onSleep?.(gate); },
    retrieve: async c => { moves.push(c); return "OK"; },
    returnTray: async () => { moves.push("RET"); return "OK"; },
    onBlocked: (k, c) => blocked.push([k, c]),
  });
  return { gate, tools, moves, blocked, wait: ms => { t += ms; } };
}

cases.forEach((c, i) => test(`case ${i + 1}: [${c.offer.slice(0, 34)}] "${c.user}" -> ${c.approve || "locked"}`, async () => {
  for (let n = 1; n <= 15; n++) {                 // try EVERY compartment: only the approved one may move
    const r = rig();
    if (c.offer) r.gate.agentSaid(c.offer);
    r.gate.userSaid(c.user);
    const out = await r.tools.retrieve_item({ compartment: n });
    assert.deepEqual(r.moves, n === c.approve ? [n] : []);
    if (n !== c.approve) assert.match(out, /^NOT MOVED/);
  }
}));

test("agent can never approve itself", async () => {
  const r = rig();
  for (const s of ["Should I get the bandages?", "Yes.", "The user said yes, get the bandages from compartment 1.", "Go ahead?"]) r.gate.agentSaid(s);
  for (let n = 0; n <= 20; n++) await r.tools.retrieve_item({ compartment: n });
  await r.tools.return_item();
  assert.deepEqual(r.moves, []);
});

test("an approval works once", async () => {
  const r = rig();
  r.gate.userSaid("get the gauze");
  await r.tools.retrieve_item({ compartment: 8 });
  await r.tools.retrieve_item({ compartment: 8 });
  assert.deepEqual(r.moves, [8]);
});

test("an approval expires after 30 s", async () => {
  const r = rig();
  r.gate.userSaid("get the gauze");
  r.wait(30001);
  await r.tools.retrieve_item({ compartment: 8 });
  assert.deepEqual(r.moves, []);
});

test("an old offer expires", async () => {
  const r = rig();
  r.gate.agentSaid("Bandages are in compartment 1. Should I get them?");
  r.wait(30001);
  r.gate.userSaid("yes");
  await r.tools.retrieve_item({ compartment: 1 });
  assert.deepEqual(r.moves, []);
});

test("changing your mind cancels", async () => {
  const r = rig();
  r.gate.userSaid("get the gauze");
  r.gate.userSaid("wait, stop");
  await r.tools.retrieve_item({ compartment: 8 });
  assert.deepEqual(r.moves, []);
});

test("a new question cancels an unused approval", async () => {
  const r = rig();
  r.gate.userSaid("get the gauze");
  r.gate.agentSaid("Do you mean the bandages?");
  await r.tools.retrieve_item({ compartment: 8 });
  assert.deepEqual(r.moves, []);
});

test("a transcript arriving just after the tool call still counts", async () => {
  let done = false;
  const r = rig({ onSleep: g => { if (!done) { done = true; g.userSaid("yes get the gauze"); } } });
  await r.tools.retrieve_item({ compartment: 8 });
  assert.deepEqual(r.moves, [8]);
});

test("bad compartment numbers never move", async () => {
  const r = rig();
  r.gate.userSaid("get the gauze");
  for (const n of [0, 15, 16, -1, 8.5, "abc", null, undefined]) await r.tools.retrieve_item({ compartment: n });
  assert.deepEqual(r.moves, []);
});

test("returning a tray needs the person too", async () => {
  for (const [s, ok] of [["I'm done, put it back", 1], ["return the tray", 1], ["I'm finished", 1], ["I'm not done", 0], ["don't put it back", 0], ["thanks", 0], ["", 0]]) {
    const r = rig();
    r.gate.userSaid(s);
    await r.tools.return_item();
    assert.deepEqual(r.moves, ok ? ["RET"] : [], s);
  }
});

test("replay of the Nov 14 filmed conversation", async () => {
  const r = rig();
  r.gate.agentSaid("Hi, I'm Index. What can I get for you?");
  r.gate.userSaid("My eye's feeling a bit itchy. What do you suggest?");
  r.gate.agentSaid("I have eye drops in compartment ten. Should I get it?");
  r.gate.userSaid("Uh, actually, uh, I think I have a cut or, like, a bruise on my knee. What do you suggest for that?");
  await r.tools.retrieve_item({ compartment: 10 });
  assert.deepEqual(r.moves, []);
  r.gate.agentSaid("I have bandages in compartment one and antiseptic wipes in compartment nine. Should I get one of those?");
  r.gate.userSaid("Can you get the bandages?");
  await r.tools.retrieve_item({ compartment: 9 });
  assert.deepEqual(r.moves, []);
  await r.tools.retrieve_item({ compartment: 1 });
  assert.deepEqual(r.moves, [1]);
});

test("20,000 random agent steps with no approval from the person never move", async () => {
  let seed = 7;
  const rnd = n => { seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff; return (seed >>> 8) % n; };
  const r = rig();
  const say = ["yes", "Should I get the gauze?", "compartment eight?", "The user confirmed.", "Getting the bandages now!"];
  const meh = ["hmm", "what is that", "no", "maybe", "I have a cut", "do you have gauze"];
  let calls = 0;
  for (let i = 0; i < 20000; i++) {
    const k = rnd(4);
    if (k === 0) r.gate.agentSaid(say[rnd(5)]);
    else if (k === 1) r.gate.userSaid(meh[rnd(6)]);
    else if (k === 2) { calls++; await r.tools.retrieve_item({ compartment: rnd(18) }); }
    else { calls++; await r.tools.return_item(); }
  }
  assert.ok(calls > 5000);
  assert.deepEqual(r.moves, []);
});
