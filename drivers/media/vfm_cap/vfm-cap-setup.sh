#!/bin/sh
#
# vfm-cap-setup.sh — Insert/remove vfm_cap from the VFM video pipeline
#
# Usage:
#   vfm-cap-setup.sh insert   — Load module and insert into tvpath
#   vfm-cap-setup.sh remove   — Remove from tvpath and unload module
#   vfm-cap-setup.sh status   — Show current VFM map and module state
#   vfm-cap-setup.sh reload   — Remove then insert (useful after recompile)
#
# The default VFM tvpath is:
#   vdin0 → amlvideo2.0 → deinterlace → amvideo
#
# After insert (vfm_cap placed right after vdin0):
#   vdin0 → vfm_cap → amlvideo2.0 → deinterlace → amvideo
#

VFM_MAP="/sys/class/vfm/map"
MODULE_NAME="vfm_cap"
MAP_NAME="tvpath"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

check_root() {
    [ "$(id -u)" -eq 0 ] || die "Must be run as root"
}

module_loaded() {
    lsmod | grep -q "^${MODULE_NAME}" 2>/dev/null
}

get_current_tvpath() {
    if [ -r "$VFM_MAP" ]; then
        grep "${MAP_NAME}" "$VFM_MAP" 2>/dev/null
    fi
}

# Extract the chain components from the tvpath line
# e.g. "[07]  tvpath { vdin0(1) amlvideo2.0(1) deinterlace(1) amvideo}" -> "vdin0 amlvideo2.0 deinterlace amvideo"
get_chain() {
    get_current_tvpath | sed 's/.*{//;s/}.*//;s/([0-9]*)//g' | xargs
}

has_vfm_cap_in_path() {
    get_current_tvpath | grep -q "vfm_cap"
}

do_status() {
    echo "=== VFM Map ==="
    if [ -r "$VFM_MAP" ]; then
        cat "$VFM_MAP"
    else
        echo "(cannot read $VFM_MAP)"
    fi

    echo ""
    echo "=== Module State ==="
    if module_loaded; then
        echo "${MODULE_NAME} is loaded"
        lsmod | grep "^${MODULE_NAME}"
    else
        echo "${MODULE_NAME} is NOT loaded"
    fi

    echo ""
    echo "=== Device Node ==="
    # Look for both /dev/video_cap and any /dev/videoN with vfm_cap name
    if [ -e /dev/video_cap ]; then
        ls -la /dev/video_cap
    else
        echo "/dev/video_cap symlink does not exist"
    fi
    for d in /sys/class/video4linux/video*; do
        if [ -f "$d/name" ] && grep -q "vfm_cap" "$d/name" 2>/dev/null; then
            devname=$(basename "$d")
            echo "Found /dev/$devname (name=$(cat "$d/name"))"
        fi
    done

    echo ""
    echo "=== Module Sysfs ==="
    if [ -d "/sys/module/${MODULE_NAME}" ]; then
        echo "Parameters:"
        for f in /sys/module/${MODULE_NAME}/parameters/*; do
            [ -f "$f" ] && echo "  $(basename "$f") = $(cat "$f")"
        done
    else
        echo "(module not loaded, no sysfs entries)"
    fi
}

do_insert() {
    check_root

    # Load module if not already loaded
    if ! module_loaded; then
        echo "Loading ${MODULE_NAME} module..."
        modprobe "${MODULE_NAME}" 2>/dev/null || \
            insmod "/lib/modules/$(uname -r)/kernel/amlogic/vfm_cap/${MODULE_NAME}.ko" \
            || die "Failed to load ${MODULE_NAME} module"
        echo "Module loaded."
    else
        echo "Module already loaded."
    fi

    # Check if already in path
    if has_vfm_cap_in_path; then
        echo "vfm_cap already in tvpath."
        return 0
    fi

    # Get current chain and build new chain with vfm_cap after vdin0
    CURRENT_CHAIN=$(get_chain)
    if [ -z "$CURRENT_CHAIN" ]; then
        die "Cannot read current tvpath chain"
    fi
    echo "Current chain: $CURRENT_CHAIN"

    # Insert vfm_cap right after vdin0
    NEW_CHAIN=$(echo "$CURRENT_CHAIN" | sed 's/vdin0/vdin0 vfm_cap/')
    echo "New chain:     $NEW_CHAIN"

    # Remove existing tvpath
    echo "Removing existing tvpath..."
    echo "rm ${MAP_NAME}" > "$VFM_MAP" || die "Failed to remove tvpath"

    # Small delay to let VFM settle
    sleep 0.2

    # Add new tvpath with vfm_cap
    echo "Adding tvpath with vfm_cap..."
    echo "add ${MAP_NAME} ${NEW_CHAIN}" > "$VFM_MAP" || die "Failed to add tvpath"

    # Verify
    echo ""
    echo "Verifying..."
    if has_vfm_cap_in_path; then
        echo "SUCCESS: vfm_cap inserted into tvpath"
        get_current_tvpath
    else
        die "FAILED: vfm_cap not found in tvpath after insertion"
    fi
}

do_remove() {
    check_root

    # Remove vfm_cap from path if present
    if has_vfm_cap_in_path; then
        CURRENT_CHAIN=$(get_chain)
        RESTORE_CHAIN=$(echo "$CURRENT_CHAIN" | sed 's/ *vfm_cap//')
        echo "Removing vfm_cap from tvpath..."
        echo "rm ${MAP_NAME}" > "$VFM_MAP" || die "Failed to remove tvpath"
        sleep 0.2
        echo "Restoring tvpath: ${RESTORE_CHAIN}"
        echo "add ${MAP_NAME} ${RESTORE_CHAIN}" > "$VFM_MAP" || die "Failed to restore tvpath"
    else
        echo "vfm_cap not in tvpath (nothing to do for VFM map)."
    fi

    # Unload module
    if module_loaded; then
        echo "Unloading ${MODULE_NAME} module..."
        rmmod "${MODULE_NAME}" || die "Failed to unload module (is a consumer still open?)"
        echo "Module unloaded."
    else
        echo "Module not loaded."
    fi

    echo "Done."
}

do_reload() {
    echo "=== Removing ==="
    do_remove
    echo ""
    echo "=== Inserting ==="
    do_insert
}

case "${1:-}" in
    insert)
        do_insert
        ;;
    remove)
        do_remove
        ;;
    status)
        do_status
        ;;
    reload)
        do_reload
        ;;
    *)
        echo "Usage: $0 {insert|remove|status|reload}"
        echo ""
        echo "  insert  — Load module and insert vfm_cap into VFM tvpath"
        echo "  remove  — Remove vfm_cap from tvpath and unload module"
        echo "  status  — Show current VFM map, module state, device info"
        echo "  reload  — Remove then insert (useful after recompile)"
        exit 1
        ;;
esac
