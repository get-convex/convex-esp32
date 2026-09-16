import { query, mutation } from "./_generated/server";
import { v } from "convex/values";

const KEY = "demo";

// Reactive: everyone (ESP32 + web) subscribes to this. It re-runs and pushes a
// new value to every subscriber whenever `tap` changes the row.
export const get = query({
  args: {},
  returns: v.object({ count: v.number(), lastActor: v.string(), updatedAt: v.number() }),
  handler: async (ctx) => {
    const row = await ctx.db
      .query("board")
      .withIndex("by_key", (q) => q.eq("key", KEY))
      .unique();
    return row
      ? { count: row.count, lastActor: row.lastActor, updatedAt: row.updatedAt }
      : { count: 0, lastActor: "—", updatedAt: 0 };
  },
});

// A tap/wave from either the device or the browser: bump the shared counter and
// record who did it. The push to all `get` subscribers is automatic.
export const tap = mutation({
  args: { who: v.string() },
  returns: v.null(),
  handler: async (ctx, { who }) => {
    const row = await ctx.db
      .query("board")
      .withIndex("by_key", (q) => q.eq("key", KEY))
      .unique();
    if (row) {
      await ctx.db.patch(row._id, { count: row.count + 1, lastActor: who, updatedAt: Date.now() });
    } else {
      await ctx.db.insert("board", { key: KEY, count: 1, lastActor: who, updatedAt: Date.now() });
    }
    return null;
  },
});
