# LanVid Monitor → App Store: owner checklist

Everything technical is prepared (bundle ID `com.manul02.lanvid.monitor`, version 2.7.0, icon, screenshots in EN + UK,
metadata, privacy policy, Fastlane, CI). What only you can do takes **five steps**.

## 1. Join the Apple Developer Program ($99 / year)
<https://developer.apple.com/programs/enroll/> — sign in with your Apple ID and pay. Approval can take up to 48 hours.

## 2. Create the app record in App Store Connect (once)
<https://appstoreconnect.apple.com> → **Apps → + → New App**: platform **iOS**, name **LanVid Monitor**,
primary language **English (U.S.)**, bundle ID **`com.manul02.lanvid.monitor`** (it is registered automatically on the
first build, or add it under *Certificates, Identifiers & Profiles → Identifiers*), SKU `lanvid-monitor`.
Apple does not allow creating the app record through the API, so this one click-through is manual.

## 3. Create an API key and copy 3 values into GitHub Secrets
App Store Connect → **Users and Access → Integrations → App Store Connect API → Team Keys → +**
(name `GitHub`, access **App Manager**, or **Admin** to let Fastlane register the bundle ID). Download the `.p8` file
(**only once!**). Then GitHub → repository **Settings → Secrets and variables → Actions → New repository secret**:

| Secret | Value |
|---|---|
| `APP_STORE_CONNECT_API_KEY_ID` | **Key ID** shown in the key list (10 characters) |
| `APP_STORE_CONNECT_ISSUER_ID` | **Issuer ID** at the top of the same page (UUID) |
| `APP_STORE_CONNECT_API_KEY_BASE64` | the `.p8` file as base64: `base64 -i AuthKey_XXXXXXXXXX.p8 \| pbcopy` (macOS) |

Optional: `APPLE_TEAM_ID` (10 characters, developer.apple.com → Membership) if the build says "Team ID unknown".

## 4. Fill in the App Review contact (needed only to *submit for review*)
Edit the four files in `mobile/ios/fastlane/metadata/review_information/` (`first_name.txt`, `last_name.txt`,
`email_address.txt`, `phone_number.txt`; replace the `FILL_IN_…` lines) — or add the secrets `REVIEW_FIRST_NAME`,
`REVIEW_LAST_NAME`, `REVIEW_EMAIL`, `REVIEW_PHONE`. Apple requires a real contact person; it is not shown publicly.

Also publish the Privacy Policy page once: repository **Settings → Pages → Source: GitHub Actions**, then run the
**Publish Pages** workflow (it publishes `…/lanvid/privacy-policy.html` and `…/support.html`).

## 5. Press the buttons
GitHub → **Actions → "iOS App Store / TestFlight (LanVid Monitor)" → Run workflow**
1. `target = testflight` → the build appears in TestFlight in ~15 minutes (install it on your iPhone to try it).
2. `target = appstore`, `submit_for_review = false` → uploads build, screenshots and all metadata.
3. In App Store Connect open the version, pick the build, answer the **App Privacy** questionnaire
   (**Data Not Collected**) and press **Submit for Review** — or run step 2 again with `submit_for_review = true`.

---
### What is where
| | |
|---|---|
| Bundle ID / version | `mobile/ios/project.yml` (`MARKETING_VERSION`; build number = `GITHUB_RUN_NUMBER`) |
| Fastlane lanes `beta`, `release`, `metadata` | `mobile/ios/fastlane/Fastfile` |
| Texts (EN + UK), categories (Music / Utilities), age rating 4+ | `mobile/ios/fastlane/metadata/`, `age_rating.json` |
| Screenshots | `mobile/ios/fastlane/screenshots/{en-US,uk}/` (regenerate: `mobile/ios/scripts/make_screenshots.sh`) |
| App icon | `mobile/ios/LanVidMonitor/Assets.xcassets/AppIcon.appiconset` (`scripts/make_icon.py`) |
| Privacy Policy / Support | `docs/privacy-policy.html`, `docs/support.html` → GitHub Pages |
| Review notes + Demo Studio | `fastlane/metadata/review_information/notes.txt` |

Run locally: `cd mobile/ios && bundle install && APP_STORE_CONNECT_API_KEY_ID=… APP_STORE_CONNECT_ISSUER_ID=…
APP_STORE_CONNECT_API_KEY_PATH=/path/AuthKey.p8 bundle exec fastlane beta`.
