# ControlProfile constructor migration fixture

From the repository root, with JDK 17+ available through `JAVA_HOME` or `PATH`:

```powershell
python scripts/test-control-profile-migration.py
```

The script compiles the current production `ControlProfile` constructor and its
actual tuning, storage-key, viewport-migration, model, trigger and trajectory
dependencies. Only Android `Context`/`SharedPreferences` storage and the unused JNI
output boundary are substituted. The JNI substitute throws if migration tries
to configure output. No Android SDK, device, Robolectric or downloaded dependency
is required; the repository's existing JSON test jar is used.

The fixture covers saved gain 2.00, 1.01, 0.95, 0.36 and 0.37. It verifies the
effective migration, bit-identical feedback math at several cadences/errors and
pursuit strengths, unchanged other saved keys and trigger/personal settings,
and a second constructor that performs no additional writes. The version-2
0.36/0.5/127 case proves the previous preset migration is not repeated.

All preference data exists only in memory. Compilation uses a script-owned
temporary directory under `build/java-profile-migration`, removed on completion.
The JSON result defaults to
`audit/report-fixes-20261025/java/profile-migration-result.json`; use `--output`
to select another report path. It records compiler/run exit codes, stdout and
the SHA-256 hashes of every compiled production/fixture source and the script.

This is a host-side constructor/storage-contract test, not an Android device or
asynchronous disk-persistence test. It does not contribute an additional group
to `scripts/test-java.py`'s separate 51-group total.
