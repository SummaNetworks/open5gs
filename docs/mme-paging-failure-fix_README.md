# MME Paging Failure Fix - Patch Application Guide

## Overview

This patch fixes critical issues in Open5GS MME paging failure handling and implements proper session cleanup with HSS notification according to 3GPP TS 29.272 Section 7.2.14.

## What This Patch Fixes

1. **Build Error**: Variable name collision causing compilation failure
2. **Missing Purge-UE-Request**: HSS not notified when paging fails
3. **NULL Pointer Crash**: Crash when S1 context is already released
4. **FSM Crash**: State machine crash during UE context cleanup

## Prerequisites

- Open5GS source code (tested with v2.7.2)
- Base branch: Any recent Open5GS version with MME paging support

## Patch Application

### Method 1: Using git apply (Recommended)

```bash
# Navigate to Open5GS source directory
cd /path/to/open5gs

# Apply the patch
git apply mme-paging-failure-fix.patch

# Check if applied successfully
git status
```

### Method 2: Using patch command

```bash
# Navigate to Open5GS source directory
cd /path/to/open5gs

# Apply the patch
patch -p1 < mme-paging-failure-fix.patch

# Check if applied successfully
git status
```

### Method 3: Using git am (preserves commit history)

```bash
# Navigate to Open5GS source directory
cd /path/to/open5gs

# Apply the patch with commit information
git am mme-paging-failure-fix.patch

# View the applied commit
git log -1
```

## Configuration

After applying the patch, configure the paging failure policy in `/etc/open5gs/mme.yaml`:

```yaml
mme:
  # ... other configuration ...

  # Paging failure policy (default: no_action)
  # - no_action: Only send UNABLE_TO_PAGE_UE cause, keep sessions (legacy behavior)
  # - delete_sessions: Delete all PDN sessions when paging fails (3GPP compliant)
  paging_failure_policy: delete_sessions

  time:
    t3413:
      value: 2      # Paging timer: 2 seconds (default)
      max_count: 1  # Paging retry count: 1 retry (default)
```

## Build and Install

```bash
# Build Open5GS
meson build --prefix=`pwd`/install
ninja -C build

# Install (requires sudo)
sudo ninja -C build install

# Restart MME service
sudo systemctl restart open5gs-mmed
```

## Verification

Check the MME startup log to confirm the policy is loaded:

```bash
sudo journalctl -u open5gs-mmed -n 50 | grep "Paging failure policy"
```

Expected output:
```
[mme] INFO: Paging failure policy set to: delete_sessions
```

When paging fails, you should see logs like:
```
[emm] WARNING: Paging to IMSI[...] failed. Stop paging
[mme] WARNING: [...] Paging failed - trigger implicit detach (policy=delete_sessions)
[mme] INFO: [...] Purge-UE-Request sent to HSS after paging failure
[mme] INFO: [...] Delete Session Request sent (paging failure, no S1 context)
```

## Modified Files

- `src/mme/mme-timer.c` - Fixed variable name collision
- `src/mme/mme-context.c` - Added paging failure policy configuration
- `src/mme/mme-context.h` - Added policy enum and configuration structure
- `src/mme/mme-gtp-path.c` - Handle NULL enb_ue case
- `src/mme/mme-path.c` - Purge-UE-Request transmission and cleanup
- `src/mme/mme-path.h` - Added cleanup function declaration
- `src/mme/mme-init.c` - Initialize paging failure timer configuration
- `src/mme/emm-sm.c` - FSM exception state transition
- `src/mme/mme-timer.c` - Timer configuration initialization
- `src/mme/mme-timer.h` - Added timer configuration function
- `configs/open5gs/mme.yaml.in` - Configuration template with policy option

## Documentation

Detailed documentation is included in the patch:
- `CLAUDE_session_deletion_on_paging_failure_r2.md` - Japanese documentation
- `CLAUDE_session_deletion_on_paging_failure_r2_en.md` - English documentation

## Testing Results

Tested on: October 15, 2025

**Before patch**:
- ❌ Paging failure causes crash
- ❌ HSS not notified
- ❌ Sessions remain active

**After patch**:
- ✅ No crashes on paging failure
- ✅ Purge-UE-Request sent to HSS
- ✅ Sessions properly cleaned up
- ✅ UE context removed correctly

## 3GPP Compliance

This patch implements 3GPP TS 29.272 Section 7.2.14 (Purge UE) requirement:
> When the MME deletes the UE context, it shall send a Purge-UE-Request to the HSS to indicate that the subscriber is no longer registered with the MME.

## Troubleshooting

### Issue: Patch fails to apply

**Solution**: Check your Open5GS version and ensure you're on a compatible base:
```bash
git log --oneline | head -5
```

### Issue: Build errors after applying patch

**Solution**: Clean build directory and rebuild:
```bash
ninja -C build clean
meson build --wipe
ninja -C build
```

### Issue: MME crashes on paging failure

**Solution**: Verify the configuration is loaded:
```bash
sudo journalctl -u open5gs-mmed -b | grep -i "paging"
```

## Rollback

If you need to rollback the patch:

```bash
# If applied with git apply or patch command
git checkout -- .

# If applied with git am
git reset --hard HEAD~1
```

## Support

For issues or questions:
- Check detailed documentation in `CLAUDE_session_deletion_on_paging_failure_r2_en.md`
- Review Open5GS logs: `/var/log/open5gs/mme.log`
- Check system logs: `sudo journalctl -u open5gs-mmed`

## License

This patch is provided under the same license as Open5GS (AGPL-3.0).

## Authors

- Generated with Claude Code
- Tested and verified on Open5GS v2.7.2
