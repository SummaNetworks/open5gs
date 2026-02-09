# ECM-IDLE Incomplete Sessions Fix - Patch Application Guide

## Patch Files

This directory contains patches for fixing crashes due to incomplete sessions in ECM-IDLE state.

### Available Patches

#### English Version (Recommended)

1. **0001-fix-ecm-idle-incomplete-sessions-en.patch** (4.1KB)
   - Standard git diff format patch with English comments
   - Contains only the code changes
   - **Recommended for production use**

2. **0001-fix-ecm-idle-incomplete-sessions-en-detailed.patch** (5.8KB)
   - Detailed patch with full commit message (English)
   - Includes problem description, solution, and impact analysis
   - **Recommended for official application**

#### Japanese Version (参考用)

3. **0001-fix-ecm-idle-incomplete-sessions-ja.patch** (4.4KB)
   - Standard patch with Japanese comments (reference only)

4. **0001-fix-ecm-idle-incomplete-sessions-ja-detailed.patch** (6.0KB)
   - Detailed patch with Japanese comments (reference only)

### Changes Summary

```
src/mme/mme-context.c | 94 insertions(+)
1 file changed, 94 insertions(+)
```

## How to Apply the Patch

### Method 1: Using git apply (Recommended)

```bash
# Navigate to the open5gs-eureka directory
cd /path/to/open5gs-eureka

# Check if the patch can be applied cleanly
git apply --check patches/0001-fix-ecm-idle-incomplete-sessions-en.patch

# Apply the patch
git apply patches/0001-fix-ecm-idle-incomplete-sessions-en.patch

# Verify the changes
git diff src/mme/mme-context.c
```

### Method 2: Using git am (For detailed patch with commit)

```bash
# Apply the detailed patch as a commit (recommended for official use)
git am patches/0001-fix-ecm-idle-incomplete-sessions-en-detailed.patch

# This will create a new commit with the full commit message
```

### Method 3: Using patch command

```bash
# Apply using standard patch utility
patch -p1 < patches/0001-fix-ecm-idle-incomplete-sessions-en.patch

# Verify the changes
git diff src/mme/mme-context.c
```

## Build and Install

After applying the patch:

```bash
# Build the project
./build.sh

# Update the MME binary
./update_nf_binary.sh mme

# Verify the fix is applied
strings /usr/bin/open5gs-mmed | grep "OLD MME-UE in ECM-IDLE with incomplete sessions"
```

## Verification

### Expected Output

When the fix is triggered, you should see logs like:

```
[mme] WARN: [441216000000000] OLD MME-UE in ECM-IDLE with incomplete sessions
[mme] WARN: [441216000000000] Cleaning up old context instead of migration
[mme] DEBUG: [441216000000000] Bearer[EBI:5] without complete TEIDs (SGW:0, eNB:0)
```

### Test Scenario

1. UE performs Attach
2. Trigger eNB Reset during session establishment
3. UE sends new Attach request
4. Verify no crash occurs and new session is established successfully

## Rollback

If you need to rollback the patch:

```bash
# Using git
git apply -R patches/0001-fix-ecm-idle-incomplete-sessions-en.patch

# Or using patch command
patch -p1 -R < patches/0001-fix-ecm-idle-incomplete-sessions-en.patch

# Rebuild
./build.sh
./update_nf_binary.sh mme
```

## Language Versions

- **English patches** (`*-en.patch`): Recommended for production use. All code comments are in English.
- **Japanese patches** (`*-ja.patch`): Reference only. Code comments are in Japanese.

The functionality is identical between English and Japanese versions; only the comments differ.

## Documentation

For detailed information about this fix, see:
- `docs/fix-ecm-idle-incomplete-sessions-ja.md` (Japanese)
- `docs/fix-ecm-idle-incomplete-sessions-en.md` (English)

## Related Issues

- TAU Request with Unknown GUTI issue
- Session migration bugs in mme_ue_set_imsi()
- MME UE context inheritance problems

## Support

For issues or questions, please refer to the documentation files or contact the development team.
