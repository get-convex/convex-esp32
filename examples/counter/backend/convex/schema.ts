import { defineSchema, defineTable } from "convex/server";
import { v } from "convex/values";

// One shared "board" the ESP32 and the web both read and write. A single row
// (key "demo") holds the state; everyone subscribes to it reactively.
export default defineSchema({
  board: defineTable({
    key: v.string(),
    count: v.number(),
    lastActor: v.string(),
    updatedAt: v.number(),
  }).index("by_key", ["key"]),
});
