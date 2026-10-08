; lass_setup.iss
; ─────────────────────────────────────────────────────────────
; اسکریپت Inno Setup برای Lass Download Manager.
;
; چطور بسازیش:
;   1. Inno Setup Compiler رو نصب کن (رایگان): https://jrsoftware.org/isinfo.php
;   2. اول برنامه رو عادی build کن (cmake --build build) تا
;      build\Lass.exe موجود باشه — این اسکریپت از همون exe واقعی
;      شماره‌نسخه رو می‌خونه (GetVersionNumbersString)، پس نیازی
;      نیست شماره‌نسخه رو دستی همین‌جا هم به‌روز کنی.
;   3. این فایل رو با Inno Setup Compiler باز کن و Build کن (F9)،
;      یا از خط فرمان: iscc installer\lass_setup.iss
;   4. خروجی: installer\output\LassSetup-<version>.exe
;
; طراحی نصب: کاملاً per-user (نه Program Files، نه نیاز به Admin/
; UAC) — دقیقاً هم‌راستا با بقیه‌ی برنامه که همه‌چیزش (تنظیمات،
; رجیستری، native messaging) از قبل per-user (HKCU) بوده. این
; یعنی نصب/حذف بدون هیچ پرامپت UAC انجام می‌شه.
; ─────────────────────────────────────────────────────────────

#define MyAppExe "..\build\Lass.exe"
#define MyAppVersion GetVersionNumbersString(MyAppExe)

; ── چک نبودِ version info (جدید) ─────────────────────────────
; اگه build\Lass.exe وجود داشته باشه ولی VERSIONINFO توش embed
; نشده باشه (یعنی resource.rc کامپایل/لینک نشده — معمولاً چون
; windres موقع build در PATH نبوده، یا این یه exe قدیمی از قبل
; از اضافه شدن resource.rc به پروژه‌ست)، GetVersionNumbersString
; بی‌صدا رشته‌ی خالی برمی‌گردونه و Inno Setup فقط با یه پیام کلی
; و گیج‌کننده در آخر کامپایل ("[Setup] section must include an
; AppVersion...") fail می‌کنه که هیچ اشاره‌ای به دلیل واقعیش
; نمی‌کنه. این چک همون حالت رو زودتر و با پیام روشن می‌گیره.
#if MyAppVersion == ""
  #error "build\Lass.exe نسخه‌اش خالیه (بدون VERSIONINFO embed-شده). یعنی احتمالاً یه build قدیمیه یا موقع build شدنش resource.rc/windres کامپایل نشده. پوشه‌ی build\ رو کامل پاک کن و از اول با 'cmake -B build && cmake --build build' بسازش (نه build.bat، مگر مطمئنی windres در PATH هست) — CMakeLists.txt خودش resource.rc رو با enable_language(RC) اجباری می‌کنه، پس اگه windres نباشه همون‌جا configure fail می‌شه به‌جای این‌که بی‌صدا رد بشه."
#endif

; ── چک زمان-کامپایل (جدید) ──────────────────────────────────
; گزارش شد که extensions\ گاهی توی بسته‌ی نصب نهایی وجود نداره.
; اسکریپت [Files] پایین‌تر از نظر syntax درست بود (Source: "..\
; extensions\*" با recursesubdirs/createallsubdirs) و در بررسی
; ما هیچ ایرادی نداشت — محتمل‌ترین توضیح اینه که ISCC وقتی از
; یه چک‌اوت/کپی ناقص پروژه (بدون پوشه‌ی extensions کنار این
; اسکریپت) کامپایل بشه، بدون هیچ خطایی فقط اون Source رو
; خالی/نادیده می‌گیره. این دو چک اون حالت رو به‌جای شکست خاموش،
; به یه خطای کامپایل واضح تبدیل می‌کنن تا اگه دوباره تکرار شد
; بلافاصله معلوم بشه به‌جای این‌که فقط بعد از نصب کامل کشف بشه.
#if !FileExists("..\extensions\lass-chrome\manifest.json")
  #error "extensions\lass-chrome\manifest.json پیدا نشد. مطمئن شو پوشه‌ی extensions\ کامل و دقیقاً کنار پوشه‌ی installer\ (یعنی در ریشه‌ی پروژه) هست، بعد دوباره کامپایل کن."
#endif
#if !FileExists("..\extensions\lass-firefox\manifest.json")
  #error "extensions\lass-firefox\manifest.json پیدا نشد. مطمئن شو پوشه‌ی extensions\ کامل و دقیقاً کنار پوشه‌ی installer\ (یعنی در ریشه‌ی پروژه) هست، بعد دوباره کامپایل کن."
#endif

[Setup]
; AppId ثابت — هیچ‌وقت بین نسخه‌ها عوضش نکن، وگرنه ویندوز هر
; نصب رو یه برنامه‌ی جدا حساب می‌کنه به‌جای آپدیت.
AppId={{117066A9-3BD1-4783-90F8-95C0F7295D74}
AppName=Lass Download Manager
AppVersion={#MyAppVersion}
AppPublisher=Lass Project
AppPublisherURL=https://github.com/
DefaultDirName={localappdata}\Programs\Lass
DefaultGroupName=Lass Download Manager
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=output
OutputBaseFilename=LassSetup-{#MyAppVersion}
SetupIconFile=..\Lass.ico
UninstallDisplayIcon={app}\Lass.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"
Name: "startupicon"; Description: "Run Lass when Windows starts"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#MyAppExe}"; DestDir: "{app}"; Flags: ignoreversion
; extensions\ باید کنار خودِ exe بمونه — browser_ext_get_extension_dir
; مسیرش رو نسبت به exe در حال اجرا حساب می‌کنه (پوشه‌ی extensions
; کنار Lass.exe)، پس این پوشه باید دقیقاً همین‌جا نصب بشه.
Source: "..\extensions\*"; DestDir: "{app}\extensions"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\AI_DISCLOSURE.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Lass Download Manager"; Filename: "{app}\Lass.exe"
Name: "{group}\Uninstall Lass Download Manager"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Lass Download Manager"; Filename: "{app}\Lass.exe"; Tasks: desktopicon

[Registry]
; "اجرا هنگام روشن شدن ویندوز" — خودِ برنامه هم یه چک‌باکس مشابه
; توی Preferences داره (settings_set_startup)؛ این‌جا فقط برای
; کسی که همون لحظه‌ی نصب می‌خواد فعالش کنه، بدون نیاز به باز کردن
; برنامه و رفتن توی تنظیمات.
;
; نکته‌ی مهم (باگ واقعی که همین‌جا رفع شد): main.c وقتی exe با
; آرگومان "/minimized" اجرا بشه، پنجره‌ی اصلی رو مخفی نگه می‌داره
; و فقط tray رو نشون می‌ده — settings_set_startup در settings.c
; همیشه این فلگ رو اضافه می‌کنه، ولی نسخه‌ی قبلی این installer
; بدون "/minimized" می‌نوشت، پس هر بار ویندوز بالا می‌اومد پنجره‌ی
; اصلی هم باز می‌شد (که قبلاً، وقتی از دسکتاپ اجرا می‌شد و از خودِ
; چک‌باکس داخل برنامه فعال می‌شد، این‌طور نبود). باید همیشه دقیقاً
; با همون فرمتی که settings_set_startup می‌نویسه یکی باشه.
Root: HKCU; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "Lass Download Manager"; \
    ValueData: """{app}\Lass.exe"" /minimized"; Tasks: startupicon; Flags: uninsdeletevalue

[Run]
Filename: "{app}\Lass.exe"; Description: "Launch Lass Download Manager"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; قبل از حذف خودِ exe، ورودی‌های native messaging مرورگرها رو پاک
; کن — وگرنه بعد از uninstall، registry به یه exe حذف‌شده اشاره
; می‌کنه و Chrome/Firefox موقع تلاش برای اجراش خطا می‌گیرن.
; "--unregister-native-host" یه mode بی‌صدا و بدون UI هست (در
; main.c اضافه شده، دقیقاً برای همین منظور).
Filename: "{app}\Lass.exe"; Parameters: "--unregister-native-host"; \
    Flags: runhidden skipifdoesntexist waituntilterminated

[UninstallDelete]
; خودِ exe/extensions رو Inno Setup به‌صورت خودکار پاک می‌کنه.
; عمداً %APPDATA%\Lass (تنظیمات، لاگ، lass_host.json) و
; HKCU\Software\Lass\Settings رو حذف نمی‌کنیم — قرارداد رایجه که
; داده‌ی کاربر با uninstall از بین نره (اگه بعداً دوباره نصب کرد،
; تنظیماتش سرجاشه). اگه یه روز خواستی uninstall کامل (پاک‌سازی
; تمام و کمال) هم داشته باشی، بگو تا یه گزینه‌ی جدا (مثلاً یه
; checkbox "remove settings too") اضافه کنم.

[Code]
// ── تشخیص ارتقا از نصب قبلی (جدید) ──────────────────────────
// چون AppId ثابته (طبق کامنت بالای [Setup]، هیچ‌وقت عوض نمی‌شه)،
// اگه نسخه‌ی قبلی همین برنامه از قبل نصب باشه، Inno Setup خودش
// به‌طور خودکار entry نصب قبلی رو زیر همین AppId توی رجیستری
// آپدیت می‌کنه (رفتار پیش‌فرض خودِ Inno، نیازی به کد اضافه
// نداره) — چیزی که این‌جا اضافه شده فقط برای عوض کردن *متن*
// ویزارده (Install → Upgrade) تا کاربر با چشم خودش ببینه داره
// ارتقا می‌ده نه یه نصب تازه؛ رفتار واقعی نصب (کپی فایل‌ها و
// جایگزینی exe قدیمی) بدون این تغییر هم از قبل درست بود.
//
// تشخیص: چون این برنامه همیشه per-user نصب می‌شه (PrivilegesRequired
// =lowest، بدون UAC — طبق طراحی مستندشده)، entry نصب قبلی همیشه
// زیر HKCU می‌مونه، نه HKLM.
var
  IsUpgrade: Boolean;

function InitializeSetup(): Boolean;
begin
  IsUpgrade := RegKeyExists(HKEY_CURRENT_USER,
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\{117066A9-3BD1-4783-90F8-95C0F7295D74}_is1');
  Result := True;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if not IsUpgrade then
    Exit;
  if CurPageID = wpWelcome then
  begin
    WizardForm.WelcomeLabel1.Caption := 'Welcome to the Lass Download Manager Upgrade Wizard';
    WizardForm.WelcomeLabel2.Caption :=
      'An older version of Lass Download Manager was found on this computer.' + #13#10 + #13#10 +
      'This wizard will upgrade it to version {#MyAppVersion}. Your settings, download history, and browser extension registration will be kept.' + #13#10 + #13#10 +
      'It is recommended that you close any other applications before continuing.';
  end
  else if CurPageID = wpReady then
  begin
    WizardForm.PageNameLabel.Caption := 'Ready to Upgrade';
    WizardForm.PageDescriptionLabel.Caption := 'Setup is now ready to upgrade Lass Download Manager on this computer.';
    WizardForm.NextButton.Caption := '&Upgrade';
  end;
end;

// ── چک زمان-نصب (جدید) ──────────────────────────────────────
// حتی با چک کامپایل بالا (که مطمئن می‌شه extensions\ کنار خودِ
// این اسکریپت وجود داره)، این یه لایه‌ی دفاعی دومه: بعد از
// کپی شدن فایل‌ها، مطمئن می‌شه هر دو extension واقعاً توی پوشه‌ی
// نصب هم فرود اومدن (مثلاً اگه AV یا یه دلیل runtime دیگه یکی
// از فایل‌ها رو silent block کنه). اگه چیزی کم بود، به‌جای
// این‌که کاربر بعداً فقط با "extension کار نمی‌کنه" روبه‌رو بشه،
// همون لحظه‌ی نصب یه پیام واضح می‌بینه.
procedure CurStepChanged(CurStep: TSetupStep);
var
  missing: String;
begin
  if CurStep = ssPostInstall then
  begin
    missing := '';
    if not FileExists(ExpandConstant('{app}\extensions\lass-chrome\manifest.json')) then
      missing := missing + '  - extensions\lass-chrome\manifest.json' + #13#10;
    if not FileExists(ExpandConstant('{app}\extensions\lass-firefox\manifest.json')) then
      missing := missing + '  - extensions\lass-firefox\manifest.json' + #13#10;
    if missing <> '' then
      MsgBox('Setup finished, but these extension files were not copied:' + #13#10 + #13#10 +
             missing + #13#10 +
             'Without these files, browser sync will not work. Please run Setup again; if this keeps happening, your antivirus is likely blocking those files.',
             mbError, MB_OK);
  end;
end;
