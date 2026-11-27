# LanVid Camera → App Store & TestFlight: 5-Step Owner Checklist

Everything technical is ready (`com.manul02.lanvid.camera`, version `3.1.0`, 1024×1024 RGB AppIcon, EN + UK App Store screenshots, metadata, privacy manifest, GitHub Pages policy, Fastlane, and GitHub Actions workflow). After enrolling in the Apple Developer Program, follow these **5 steps**:

## 1. Enroll in the Apple Developer Program ($99 / year)
<https://developer.apple.com/programs/enroll/> — enroll with your Apple ID and wait for activation (typically up to 48 hours).

## 2. Create the App Record in App Store Connect (once)
Open <https://appstoreconnect.apple.com> → **Apps → + → New App**:
- **Platform:** iOS
- **Name:** `LanVid Camera`
- **Primary Language:** English (U.S.)
- **Bundle ID:** `com.manul02.lanvid.camera`
- **SKU:** `lanvid-camera`

## 3. Add App Store Connect API Key to GitHub Secrets
App Store Connect → **Users and Access → Integrations → App Store Connect API → Team Keys → +** (role **App Manager** or **Admin**), download the `.p8` file once, and add to GitHub **Settings → Secrets and variables → Actions**:
- `APP_STORE_CONNECT_API_KEY_ID` — 10-character Key ID
- `APP_STORE_CONNECT_ISSUER_ID` — Issuer UUID
- `APP_STORE_CONNECT_API_KEY_BASE64` — `base64 -i AuthKey_XXXXXXXXXX.p8 | pbcopy`
- *(Optional)* `APPLE_TEAM_ID` — 10-character Team ID from developer.apple.com → Membership

## 4. Set App Review Contact & Publish Privacy Policy Page
- Fill in `REVIEW_FIRST_NAME`, `REVIEW_LAST_NAME`, `REVIEW_EMAIL`, `REVIEW_PHONE` in GitHub Secrets (or edit `mobile/ios_camera/fastlane/metadata/review_information/*.txt`).
- Ensure GitHub Pages is enabled (**Settings → Pages → Source: GitHub Actions**) so `https://manul02-ndn.github.io/lanvid/camera-privacy.html` and `support.html` are live.

## 5. Trigger Release in GitHub Actions
Open GitHub → **Actions → "iOS App Store / TestFlight (LanVid Camera)" → Run workflow**:
1. Run `target = testflight` to upload a signed build to TestFlight and test on your iPhone.
2. Run `target = appstore`, `submit_for_review = false` to upload the binary, screenshots (`en-US` & `uk`), and all metadata.
3. In App Store Connect, complete **App Privacy → Data Not Collected** and click **Submit for Review** (or run step 2 with `submit_for_review = true`).
