## Deferred / suggested ideas so far

Here’s the list of things we’ve mentioned but not yet implemented (mostly from earlier messages):

1. **Better validation messages**
   - Return structured errors like `{"error": "invalid_owner", "details": "must be alphanumeric or underscore"}`

2. **Rate limiting / abuse protection** (very light, e.g. 100 req/min per IP)
   → Optional — probably overkill for localhost/personal use

3. **API documentation / OpenAPI/Swagger**
   → A simple `/docs` endpoint or static JSON spec so future you (or GitHub visitors) understand the API

4. **Versioning**
   - `/v1/generate`, `/v1/confirm`, etc. — easy to add later if you ever break compatibility

5. **Environment-based config**
   - Read port, secret, log path from env vars or config file instead of hard-coded

6. **Graceful shutdown improvements**
   - Already decent, but could add a short delay or broadcast “shutting down” to `/health`

7. **Demo clients** (your person app + book app idea)
   → Yes — even very simple PHP pages showing:
     - “Create new person” → calls `/generate` → stores ID
     - “Confirm person created” → calls `/confirm`
     - “View person” → calls `/get_id/<id>`
     - Same for books

8. **Minor polish**
   - Custom 503 message during suspend: `{"status":"suspended","message":"Temporarily unavailable for maintenance"}`
   - Add `X-Powered-By: id-registry v0.1` header (fun for demos)
   - Log startup banner with version/date (console + log file)

### Additional suggestions, some by me, some forgotten by Grok, etc.

- Rename the gui to config.
- Move DBPath from the Windows Registry to a config file.
- Create executable as Service in stead of CLI app, thus obviating need for NSSM.
- Make sure to flush the database for /suspend.