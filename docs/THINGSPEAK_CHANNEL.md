# ThingSpeak channel reference

- **URL:** https://thingspeak.mathworks.com/channels/3521581
- **Channel ID:** `3521581`
- **Status:** Owner-provided reference only; channel fields, access, and measurements not verified.
- **Expected gateway field use:** Based on `docs/ARCHITECTURE.md`, fields 1–4 and 6–8 are used by the gateway. This is a *code mapping*, not a verified channel configuration.

The gateway uses a **write API key**, not a hard-coded channel number, to post to ThingSpeak. Do not put real write/TalkBack keys into this public repository. To test later, use newly issued keys in local ignored `gateway/config.json` or environment variables. Rotate any credentials embedded in older coursework archives.
