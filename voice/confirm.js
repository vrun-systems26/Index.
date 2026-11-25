// Confirmation gate: the voice agent cannot move the robot unless the PERSON said so.
// Approval comes only from the person's own transcript (or a tap), never from the agent's words or tool calls.
// Deliberately strict: anything unclear, negative or ambiguous does not approve, and the agent has to ask again.

const NUM = { one: 1, two: 2, three: 3, four: 4, five: 5, six: 6, seven: 7, eight: 8, nine: 9, ten: 10, eleven: 11, twelve: 12, thirteen: 13, fourteen: 14, fifteen: 15 };
const ALIAS = { "band aid": "bandages", "bandaid": "bandages", "wipe": "antiseptic wipes", "tape": "duct tape", "battery": "aa batteries", "batteries": "aa batteries" };
const NO = /\b(no|nope|nah|not|dont|cant|wont|never|stop|cancel|wait|hold on|forget it|nevermind|instead|actually|but|rather)\b/;
const YES = /\b(yes|yeah|yep|yup|sure|ok|okay|please do|go ahead|do it|sounds good|correct|thats right)\b/;
// must be a command TO the robot, so "my friend can get gauze" does not count
const GET = /^((hey|hi|index|please|so|um|uh|now|then|yes|yeah|yep|sure|ok|okay|can you|could you|would you|will you|id like you to|i want you to) )*(get|bring|grab|fetch|retrieve)\b/;
const ASKING = /\b(do (you|we) have|is there|are there|what|which|where|how|why|who|when|did you|should)\b/;
const BACK = /\b(put (it|that|this|them|the tray) (back|away)|put back|return (it|that|this|the tray|tray)|im done|i am done|im finished|i am finished|all done)\b/;

export function norm(s) {
  return String(s).toLowerCase().replace(/[’']/g, "").replace(/[^a-z0-9 ]+/g, " ").replace(/\s+/g, " ").trim();
}
const stem = w => (w.length > 3 && w.endsWith("s") ? w.slice(0, -1) : w);
const stems = s => ` ${norm(s).split(" ").map(stem).join(" ")} `;

export class ConfirmGate {
  constructor(inventory, { ttl = 30000, now = () => Date.now() } = {}) {
    this.inv = inventory;
    this.ttl = ttl;
    this.now = now;
    this.reset();
  }

  reset() {
    this.offer = { list: [], t: -Infinity };
    this.ok = null;
    this.back = -Infinity;
  }

  // every stocked compartment named in a sentence, by item name or "compartment N"
  mentions(text) {
    const s = stems(text), out = new Set();
    this.inv.forEach((name, i) => { if (name && s.includes(stems(name))) out.add(i + 1); });
    for (const [a, item] of Object.entries(ALIAS)) {
      if (!s.includes(stems(a))) continue;
      const i = this.inv.findIndex(n => norm(n) === item);
      if (i >= 0) out.add(i + 1);
    }
    for (const m of norm(text).matchAll(/\b(?:compartment|number) (\w+)/g)) {
      const n = NUM[m[1]] || parseInt(m[1]);
      if (n >= 1 && n <= this.inv.length && this.inv[n - 1]) out.add(n);
    }
    return [...out];
  }

  // a new question from the agent replaces the old offer and any unused approval
  agentSaid(text) {
    if (!String(text).includes("?")) return;
    this.offer = { list: this.mentions(text), t: this.now() };
    this.ok = null;
    this.back = -Infinity;
  }

  // the newest thing the person says always wins; returns a short reason for the log
  userSaid(text) {
    const s = norm(text);
    this.ok = null;
    this.back = -Infinity;
    if (!s) return "nothing heard";
    if (NO.test(s)) {
      this.offer = { list: [], t: -Infinity };
      return "negative or unsure: not approved";
    }
    if (BACK.test(s)) {
      this.back = this.now();
      return "return approved";
    }
    const named = this.mentions(text), yes = YES.test(s);
    const offer = this.now() - this.offer.t <= this.ttl ? this.offer.list : [];
    if (named.length > 1) return "more than one item named: not approved";
    if (named.length === 1) {
      const c = named[0];
      if (GET.test(s) && !ASKING.test(s)) return this.approve(c);
      if (yes && offer.includes(c)) return this.approve(c);
      return "item named but not requested: not approved";
    }
    if (yes && offer.length === 1) return this.approve(offer[0]);
    if (yes && offer.length > 1) return "yes to more than one option: not approved";
    return "no clear yes: not approved";
  }

  approve(c) {
    this.ok = { c, t: this.now() };
    return `approved compartment ${c}`;
  }

  // single use
  take(c) {
    const good = !!this.ok && this.ok.c === Number(c) && this.now() - this.ok.t <= this.ttl;
    if (good) {
      this.ok = null;
      this.offer = { list: [], t: -Infinity };
    }
    return good;
  }

  takeReturn() {
    const good = this.now() - this.back <= this.ttl;
    if (good) this.back = -Infinity;
    return good;
  }
}

// Wraps the agent's movement tools. The transcript can arrive just after the tool call, so wait briefly.
export function guardTools({ gate, retrieve, returnTray, inventory, onBlocked = () => {}, graceMs = 1500, sleep = ms => new Promise(r => setTimeout(r, ms)) }) {
  const until = async f => {
    for (let t = 0; ; t += 100) {
      if (f()) return true;
      if (t >= graceMs) return false;
      await sleep(100);
    }
  };
  return {
    retrieve_item: async ({ compartment } = {}) => {
      const c = Number(compartment);
      if (compartment === null || !Number.isInteger(c) || c < 1 || c > inventory.length || !inventory[c - 1]) {
        return `NOT MOVED: compartment ${compartment} is not a stocked compartment.`;
      }
      if (await until(() => gate.take(c))) return retrieve(c, "voice agent");
      onBlocked("retrieve", c);
      return `NOT MOVED: the person has not confirmed compartment ${c} (${inventory[c - 1]}). Ask "Should I get the ${inventory[c - 1]} from compartment ${c}?" and wait for a clear yes. Do not say it is on its way.`;
    },
    return_item: async () => {
      if (await until(() => gate.takeReturn())) return returnTray("voice agent");
      onBlocked("return", 0);
      return "NOT MOVED: the person has not asked to put the tray back. Ask if they are done and wait for them to say so.";
    },
  };
}
