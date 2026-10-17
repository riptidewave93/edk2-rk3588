#!/bin/bash

function _help(){
    echo
    echo "Build EDK2 for Rockchip RK3588 platforms."
    echo
    echo "Usage: build.sh [options]"
    echo
    echo "Options:"
    echo "  -d, --device DEV            Build for DEV, or 'all'."
    echo "  -r, --release MODE          Release mode for building, default is 'DEBUG', 'RELEASE' alternatively."
    echo "  -t, --toolchain TOOLCHAIN   Set toolchain, default is 'GCC'."
    echo "  --open-tfa ENABLE           Use open-source TF-A submodule. Default: ${OPEN_TFA}"
    echo "  --tfa-flags \"FLAGS\"         Flags appended to open TF-A build process."
    echo "  --optee SOURCE              OP-TEE to pack as BL32, 'vendor' or 'upstream'. Default: ${OPTEE}"
    echo "  --optee-enable              Start OP-TEE from open TF-A. Default: ${OPTEE_ENABLE}"
    echo "  --optee-flags \"FLAGS\"       Flags appended to the upstream OP-TEE build process."
    echo "  --optee-storage STORAGE     Upstream OP-TEE secure storage, 'ree', 'rpmb' or 'ree+rpmb'. Default: ${OPTEE_STORAGE}"
    echo "                              'rpmb' and 'ree+rpmb' imply --optee upstream."
    echo "  --ftpm                      Embed the fTPM TA in upstream OP-TEE. Default: ${FTPM}"
    echo "                              Implies --optee upstream --optee-enable."
    echo "  --rpmb-provision            One-time build that programs the eMMC RPMB key. Default: ${RPMB_PROVISION}"
    echo "                              Implies --optee upstream --optee-enable, needs an RPMB --optee-storage."
    echo "  --edk2-flags \"FLAGS\"        Flags appended to the EDK2 build process."
    echo "  --skip-patchsets            Skip applying upstream submodule patchsets during development."
    echo "  -C, --clean                 Clean workspace and output."
    echo "  -D, --distclean             Clean up all files that are not in repo."
    echo "  --secure-boot               Add MSFT secure boot keys. Default: ${SECUREBOOT}"
    echo "  --enable-secure-boot        Enable secure boot by default. Default: ${ENABLESECUREBOOT}"
    echo "  -h, --help                  Show this help."
    echo
    exit "${1}"
}

function _error() { echo "${@}" >&2; exit 1; }

function apply_patchset() {
    ${SKIP_PATCHSETS} && return 0

    local patches_dir="$1"
    local target_dir="$2"

    [ ! -d "${patches_dir}" ] && return 0

    if [ ! -d "${target_dir}" ]; then
        echo "Patchset target directory does not exist: ${target_dir}"
        return 1
    fi

    echo "Checking patchset ${patches_dir} for ${target_dir}"

    local patchset_name=$(basename "${patches_dir}")
    local patchset_marker="${target_dir}/.patchset_${patchset_name}"

    if [ ! -f "${patchset_marker}" ] || [ "${patches_dir}" -nt "${patchset_marker}" ]; then
        echo "Patchset needs to be (re)applied"
        if ! git -C "${target_dir}" reset --hard || ! git -C "${target_dir}" clean -xfd; then
            echo "Failed to reset git repository - aborting"
            return 1
        fi
    else
        echo "Patchset already applied - skipping"
        return 0
    fi

    local patch_file
    local patch_count=0

    for patch_file in "${patches_dir}"/*.patch; do
        [ -f "${patch_file}" ] || continue

        local patch_name=$(basename "${patch_file}")
        echo "Patch ${patch_count}: ${patch_name}"

        if patch -p1 -d "${target_dir}" < "${patch_file}"; then
            echo "  Successfully applied"
            ((patch_count++))
        else
            echo "  Failed to apply - aborting"
            return 1
        fi
    done

    touch "${patchset_marker}"

    echo "Patchset summary: ${patch_count} applied"
    return 0
}

function _build_optee() {
    echo " => Building OP-TEE"

    apply_patchset "${ROOTDIR}/optee_os-patches" "${ROOTDIR}/optee_os" || exit 1

    local OPTEE_DEBUG=0
    [ "${RELEASE_TYPE}" == "DEBUG" ] && OPTEE_DEBUG=1

    local OPTEE_MAKE=(
        make -C "${ROOTDIR}/optee_os"
        O="${WORKSPACE}/optee_os"
        PLATFORM=rockchip-${SOC_L}
        CROSS_COMPILE64="${CROSS_COMPILE}"
        CFG_ARM64_core=y
        CFG_USER_TA_TARGETS=ta_arm64
        CFG_RK_SECURE_BOOT=n
        CFG_CORE_HUK_SUBKEY_COMPAT=y
        CFG_EARLY_CONSOLE=y
        CFG_TZDRAM_START=${OPTEE_TZDRAM_START}
        CFG_TZDRAM_SIZE=${OPTEE_TZDRAM_SIZE}
        CFG_SHMEM_START=${OPTEE_SHMEM_START}
        CFG_SHMEM_SIZE=${OPTEE_SHMEM_SIZE}
        DEBUG=${OPTEE_DEBUG}
    )

    case "${OPTEE_STORAGE}" in
        rpmb) OPTEE_MAKE+=(CFG_REE_FS=n CFG_RPMB_FS=y) ;;
        ree+rpmb) OPTEE_MAKE+=(CFG_RPMB_FS=y CFG_REE_FS_INTEGRITY_RPMB=y) ;;
    esac

    if [ "${OPTEE_STORAGE}" != "ree" ]; then
        OPTEE_MAKE+=(CFG_RPMB_FS_DEV_ID=${OPTEE_RPMB_DEV_ID:-0})
        # Every RPMB file open walks the FAT, cache it instead of rereading it from the eMMC
        OPTEE_MAKE+=(CFG_RPMB_FS_CACHE_ENTRIES=128 CFG_CORE_HEAP_SIZE=196608)
    fi

    if ${RPMB_PROVISION}; then
        OPTEE_MAKE+=(CFG_RPMB_WRITE_KEY=y)
    fi

    OPTEE_MAKE+=(${OPTEE_FLAGS})

    if ${FTPM}; then
        OPTEE_MAKE+=(CFG_EARLY_TA=y)
        "${OPTEE_MAKE[@]}" ta_dev_kit

        echo " => Building fTPM TA"
        make -C "${ROOTDIR}/optee_ftpm" \
            O="${WORKSPACE}/optee_ftpm" \
            TA_DEV_KIT_DIR="${WORKSPACE}/optee_os/export-ta_arm64" \
            CFG_MS_TPM_20_REF="${ROOTDIR}/ms-tpm-20-ref" \
            CROSS_COMPILE="${CROSS_COMPILE}"

        OPTEE_MAKE+=(EARLY_TA_PATHS="${WORKSPACE}/optee_ftpm/${FTPM_UUID}.stripped.elf")
    fi

    "${OPTEE_MAKE[@]}"

    echo " => OP-TEE build done"
}

function _build_idblock() {
    echo " => Building idblock.bin"
    pushd ${WORKSPACE}

    FLASHFILES="FlashHead.bin FlashData.bin FlashBoot.bin"
    rm -f rk35*_spl_loader_*.bin idblock.bin rk35*_ddr_*.bin rk35*_usbplug*.bin UsbHead.bin ${FLASHFILES}

    DDRBIN_RKBIN=$(grep '^FlashData' ${ROOTDIR}/misc/rkbin/RKBOOT/${MINIALL_INI} | cut -d = -f 2-)
    SPL_RKBIN=$(grep '^FlashBoot' ${ROOTDIR}/misc/rkbin/RKBOOT/${MINIALL_INI} | cut -d = -f 2-)

    DDRBIN="${ROOTDIR}/misc/rkbin/${DDRBIN_RKBIN}"

    #
    # SPL v1.13 has broken SD card support!
    # Use v1.12 instead.
    #
    # SPL="${ROOTDIR}/misc/rkbin/${SPL_RKBIN}"
    SPL="${ROOTDIR}/misc/rk3588_spl_v1.12.bin"

    # Create idblock.bin
    ${ROOTDIR}/misc/tools/${MACHINE_TYPE}/mkimage -n rk3588 -T rksd -d ${DDRBIN}:${SPL} idblock.bin

    popd
    echo " => idblock.bin build done"
}

function _build_fit() {
    echo " => Building FIT"
    pushd ${WORKSPACE}

    BL31_RKBIN=$(grep '^PATH=.*_bl31_' ${ROOTDIR}/misc/rkbin/RKTRUST/${TRUST_INI} | cut -d = -f 2-)
    BL32_RKBIN=$(grep '^PATH=.*_bl32_' ${ROOTDIR}/misc/rkbin/RKTRUST/${TRUST_INI} | cut -d = -f 2-)

    BL31="${ROOTDIR}/misc/rkbin/${BL31_RKBIN}"
    BL32="${ROOTDIR}/misc/rkbin/${BL32_RKBIN}"

    if ${OPEN_TFA}; then
        BL31="${ROOTDIR}/arm-trusted-firmware/build/${TFA_PLAT}/${RELEASE_TYPE,,}/bl31/bl31.elf"
    fi

    if [ "${OPTEE}" == "upstream" ]; then
        BL32="${WORKSPACE}/optee_os/core/tee-raw.bin"
    fi

    rm -f bl31_0x*.bin ${WORKSPACE}/BL33_AP_UEFI.Fv ${SOC_L}_${DEVICE}_EFI.its

    ${ROOTDIR}/misc/extractbl31.py ${BL31}

    #
    # Emit one FIT image node per BL31 PT_LOAD segment.
    #
    # Which segments exist is up to the linker, and it moves with the TF-A
    # version: through v2.12 rk3588 produced a single writable segment plus
    # PMUSRAM, while v2.15 turns on the platform linker script and splits the
    # main one into separate read-only and read-write segments. A fixed list in
    # the .its silently drops any segment nobody thought to add -- BL31 is then
    # loaded with that part of itself missing and dies before it can report why.
    #
    # extractbl31.py names each file after its load address, zero padded to
    # eight hex digits, so sorting the names sorts them by address. The lowest
    # is where BL31 is entered and becomes the FIT's "firmware"; the rest have
    # to be listed as loadables or they are not loaded at all.
    #
    ATF_NODES="$(mktemp)"
    ATF_LOADABLES=""
    ATF_INDEX=0

    for ATF_SEG in $(ls bl31_0x*.bin 2>/dev/null | sort); do
        ATF_ADDR="${ATF_SEG#bl31_}"
        ATF_ADDR="${ATF_ADDR%.bin}"
        ATF_INDEX=$((ATF_INDEX + 1))

        cat >> "${ATF_NODES}" <<EOF
		atf-${ATF_INDEX} {
			description = "ARM Trusted Firmware";
			data = /incbin/("./${ATF_SEG}");
			type = "firmware";
			arch = "arm64";
			os = "arm-trusted-firmware";
			compression = "none";
			load = <${ATF_ADDR}>;
			hash {
				algo = "sha256";
			};
		};
EOF

        if [ ${ATF_INDEX} -gt 1 ]; then
            ATF_LOADABLES="${ATF_LOADABLES}\"atf-${ATF_INDEX}\", "
        fi
    done

    if [ ${ATF_INDEX} -eq 0 ]; then
        rm -f "${ATF_NODES}"
        _error "No BL31 segments extracted from ${BL31}"
    fi

    cp ${BL32} ${WORKSPACE}/bl32.bin
    cp ${ROOTDIR}/misc/${SOC_L}_spl.dtb ${WORKSPACE}/${DEVICE}.dtb
    cp ${WORKSPACE}/Build/${PLATFORM_NAME}/${RELEASE_TYPE}_${TOOLCHAIN}/FV/BL33_AP_UEFI.Fv ${WORKSPACE}/
    sed -e "s,@DEVICE@,${DEVICE},g" \
        -e "s|@ATF_LOADABLES@|${ATF_LOADABLES}|" \
        -e "/@ATF_IMAGES@/r ${ATF_NODES}" \
        -e "/@ATF_IMAGES@/d" \
        ${ROOTDIR}/misc/uefi_${SOC_L}.its > ${SOC_L}_${DEVICE}_EFI.its
    rm -f "${ATF_NODES}"
    ${ROOTDIR}/misc/tools/${MACHINE_TYPE}/mkimage -f ${SOC_L}_${DEVICE}_EFI.its -E ${DEVICE}_EFI.itb

    popd
    echo " => FIT build done"
}

function _pack_image() {
    _build_idblock
    _build_fit

    echo " => Building 8MB NOR FLASH IMAGE"
    cp ${WORKSPACE}/Build/${PLATFORM_NAME}/${RELEASE_TYPE}_${TOOLCHAIN}/FV/NOR_FLASH_IMAGE.fd ${WORKSPACE}/RK3588_NOR_FLASH.img
    dd if=/dev/zero of=${WORKSPACE}/RK3588_RAWEDK2.img bs=1K count=8192 conv=notrunc

    # GPT at 0x0, size:0x4400
    dd if=${ROOTDIR}/misc/rk3588_spi_nor_gpt.img of=${WORKSPACE}/RK3588_NOR_FLASH.img
    # idblock at 0x8000
    dd if=${WORKSPACE}/idblock.bin of=${WORKSPACE}/RK3588_NOR_FLASH.img bs=1K seek=32
    dd if=${WORKSPACE}/idblock.bin of=${WORKSPACE}/RK3588_RAWEDK2.img bs=1K seek=0 conv=notrunc
    # FIT Image at 0x100000
    dd if=${WORKSPACE}/${DEVICE}_EFI.itb of=${WORKSPACE}/RK3588_NOR_FLASH.img bs=1K seek=1024
    dd if=${WORKSPACE}/${DEVICE}_EFI.itb of=${WORKSPACE}/RK3588_RAWEDK2.img bs=1K seek=992 conv=notrunc
    cp ${WORKSPACE}/RK3588_NOR_FLASH.img ${ROOTDIR}/${DEVICE}_NOR_FLASH.img
    cp ${WORKSPACE}/RK3588_RAWEDK2.img ${ROOTDIR}/${DEVICE}_RAWEDK2.img
}

function _build(){
    local DEVICE="${1}"; shift

    #
    # Grab platform parameters
    #
    if [ -f "configs/${DEVICE}.conf" ]
    then source "configs/${DEVICE}.conf"
    else _error "Device configuration not found"
    fi
    if [ -f "configs/${SOC}.conf" ]
    then source "configs/${SOC}.conf"
    else _error "SoC configuration not found"
    fi
    typeset -l SOC_L="$SOC"

    rm -f "${OUTDIR}/RK35*_NOR_FLASH.img"

    #
    # Build TF-A
    #
    if ${OPEN_TFA}; then
        apply_patchset "${ROOTDIR}/arm-trusted-firmware-patches" "${ROOTDIR}/arm-trusted-firmware" || exit 1

        pushd arm-trusted-firmware

        if [ ${RELEASE_TYPE} == "DEBUG" ]; then
            DEBUG=1
        else
            DEBUG=0
        fi

        TFA_OPTEE_FLAGS=""
        if ${OPTEE_ENABLE}; then
            TFA_OPTEE_FLAGS="SPD=opteed ROCKCHIP_BL32_BASE=${OPTEE_TZDRAM_START}"
        fi

        make PLAT=${TFA_PLAT} DEBUG=${DEBUG} all ${TFA_OPTEE_FLAGS} ${TFA_FLAGS}

        popd
    fi

    if [ "${OPTEE}" == "upstream" ]; then
        _build_optee
    fi

    #
    # Build EDK2
    #
    apply_patchset "${ROOTDIR}/edk2-patches" "${ROOTDIR}/edk2" || exit 1
    apply_patchset "${ROOTDIR}/devicetree/mainline/patches" "${ROOTDIR}/devicetree/mainline/upstream" || exit 1

    [ -d "${WORKSPACE}/Conf" ] || mkdir -p "${WORKSPACE}/Conf"

    export GCC_AARCH64_PREFIX="${CROSS_COMPILE}"
    export CLANG38_AARCH64_PREFIX="${CROSS_COMPILE}"
    PACKAGES_PATH="${ROOTDIR}"
    PACKAGES_PATH+=":${ROOTDIR}/devicetree"
    PACKAGES_PATH+=":${ROOTDIR}/edk2"
    PACKAGES_PATH+=":${ROOTDIR}/edk2-non-osi"
    PACKAGES_PATH+=":${ROOTDIR}/edk2-platforms"
    PACKAGES_PATH+=":${ROOTDIR}/edk2-rockchip"
    PACKAGES_PATH+=":${ROOTDIR}/edk2-rockchip-non-osi"
    export PACKAGES_PATH

    make -C "${ROOTDIR}/edk2/BaseTools"
    source "${ROOTDIR}/edk2/edksetup.sh" --reconfig

    # The EDK2 RPMB client cannot program the key, so provisioning goes through Linux
    if ${FTPM} && ! ${RPMB_PROVISION}; then
        EDK2_FLAGS="${EDK2_FLAGS} -D RK_FTPM_ENABLE=TRUE"
    fi

    if "${SECUREBOOT}"; then
        if ! [ -d "${ROOTDIR}/keys" ]; then
            mkdir -p "${ROOTDIR}/keys"
        fi
        # Download keys if not present
        if ! [ -f "${ROOTDIR}/keys/pk.cer" ]; then
            openssl req -new -x509 -newkey rsa:2048 -subj "/CN=Rockchip Platform Key/" -keyout /dev/null -outform DER -out keys/pk.cer -days 7300 -nodes -sha256
        fi
        if ! [ -f "${ROOTDIR}/keys/ms_kek1.cer" ]; then
            curl -L https://go.microsoft.com/fwlink/?LinkId=321185 -o keys/ms_kek1.cer
        fi
        if ! [ -f "${ROOTDIR}/keys/ms_kek2.cer" ]; then
            curl -L https://go.microsoft.com/fwlink/?linkid=2239775 -o keys/ms_kek2.cer
        fi
        if ! [ -f "${ROOTDIR}/keys/ms_db1.cer" ]; then
            curl -L https://go.microsoft.com/fwlink/?LinkId=321192 -o keys/ms_db1.cer
        fi
        if ! [ -f "${ROOTDIR}/keys/ms_db2.cer" ]; then
            curl -L https://go.microsoft.com/fwlink/?LinkId=321194 -o keys/ms_db2.cer
        fi
        if ! [ -f "${ROOTDIR}/keys/ms_db3.cer" ]; then
            curl -L https://go.microsoft.com/fwlink/?linkid=2239776 -o keys/ms_db3.cer
        fi
        if ! [ -f "${ROOTDIR}/keys/ms_db4.cer" ]; then
            curl -L https://go.microsoft.com/fwlink/?linkid=2239872 -o keys/ms_db4.cer
        fi
        if ! [ -f "${ROOTDIR}/keys/ms_db5.cer" ]; then
            curl -L https://go.microsoft.com/fwlink/?linkid=2284009 -o keys/ms_db5.cer
        fi
        if ! [ -f "${ROOTDIR}/keys/arm64_dbx.bin" ]; then
            curl -L https://uefi.org/sites/default/files/resources/dbxupdate_arm64.bin -o keys/arm64_dbx.bin
        fi
        EDK2_FLAGS="${EDK2_FLAGS} \
              -D SECURE_BOOT_ENABLE=TRUE \
              -D DEFAULT_KEYS=TRUE \
              -D PK_DEFAULT_FILE=keys/pk.cer \
              -D KEK_DEFAULT_FILE1=keys/ms_kek1.cer \
              -D KEK_DEFAULT_FILE2=keys/ms_kek2.cer \
              -D DB_DEFAULT_FILE1=keys/ms_db1.cer \
              -D DB_DEFAULT_FILE2=keys/ms_db2.cer \
              -D DB_DEFAULT_FILE3=keys/ms_db3.cer \
              -D DB_DEFAULT_FILE4=keys/ms_db4.cer \
              -D DB_DEFAULT_FILE5=keys/ms_db5.cer \
              -D DBX_DEFAULT_FILE1=keys/arm64_dbx.bin"

        if "${ENABLESECUREBOOT}"; then
            EDK2_FLAGS="${EDK2_FLAGS} \
              -D RK3588_SECURE_BOOT_DEFAULT_ENABLE=TRUE"
        fi
    fi

    build \
        -s \
        -n 0 \
        -a AARCH64 \
        -t "${TOOLCHAIN}" \
        -p "${ROOTDIR}/${DSC_FILE}" \
        -b "${RELEASE_TYPE}" \
        -D FIRMWARE_VER="${GIT_COMMIT}" \
        -D NETWORK_ALLOW_HTTP_CONNECTIONS=TRUE \
        -D NETWORK_ISCSI_ENABLE=TRUE \
        -D INCLUDE_TFTP_COMMAND=TRUE \
        --pcd gRockchipTokenSpaceGuid.PcdFitImageFlashAddress=0x100000 \
        ${EDK2_FLAGS}

    #
    # Compile final image
    #
    _pack_image

    echo "Build done: RK3588_NOR_FLASH.img"
}

function _clean() { rm --one-file-system --recursive --force "${OUTDIR}"/workspace "${OUTDIR}"/RK3588_*.img; }
function _distclean() { if [ -d .git ]; then git clean -xdf; else _clean; fi; }

#
# Default variables
#
typeset -l DEVICE
typeset -u RELEASE_TYPE
DEVICE=""
RELEASE_TYPE=DEBUG
TOOLCHAIN=GCC
OPEN_TFA=true
TFA_FLAGS=""
OPTEE=vendor
OPTEE_SET=false
OPTEE_ENABLE=false
OPTEE_FLAGS=""
OPTEE_STORAGE=ree
FTPM=false
RPMB_PROVISION=false
FTPM_UUID=bc50d971-d4c9-42c4-82cb-343fb7f37896
EDK2_FLAGS=""
SKIP_PATCHSETS=false
CLEAN=false
DISTCLEAN=false
OUTDIR="${PWD}"
SECUREBOOT=false
ENABLESECUREBOOT=false

#
# Get options
#
OPTS=$(getopt -o "d:r:t:CDh" -l "device:,release:,toolchain:,open-tfa:,tfa-flags:,optee:,optee-enable,optee-flags:,optee-storage:,ftpm,rpmb-provision,edk2-flags:,skip-patchsets,clean,distclean,secure-boot,enable-secure-boot,help" -n build.sh -- "${@}") || _help $?
eval set -- "${OPTS}"
while true; do
    case "${1}" in
        -d|--device) DEVICE="${2}"; shift 2 ;;
        -r|--release) RELEASE_TYPE="${2}"; shift 2 ;;
        -t|--toolchain) TOOLCHAIN="${2}"; shift 2 ;;
        --open-tfa) OPEN_TFA="${2}"; shift 2 ;;
        --tfa-flags) TFA_FLAGS="${2}"; shift 2 ;;
        --optee) OPTEE="${2}"; OPTEE_SET=true; shift 2 ;;
        --optee-enable) OPTEE_ENABLE=true; shift ;;
        --optee-flags) OPTEE_FLAGS="${2}"; shift 2 ;;
        --optee-storage) OPTEE_STORAGE="${2}"; shift 2 ;;
        --ftpm) FTPM=true; shift ;;
        --rpmb-provision) RPMB_PROVISION=true; shift ;;
        --edk2-flags) EDK2_FLAGS="${2}"; shift 2 ;;
        --skip-patchsets) SKIP_PATCHSETS=true; shift ;;
        -C|--clean) CLEAN=true; shift ;;
        -D|--distclean) DISTCLEAN=true; shift ;;
        --secure-boot) SECUREBOOT=true; shift ;;
        --enable-secure-boot) ENABLESECUREBOOT=true; shift ;;
        -h|--help) _help 0; shift ;;
        --) shift; break ;;
        *) break ;;
    esac
done
if [[ -n "${@}" ]]; then
    echo "Invalid additional arguments '${@}'"
    _help 1
fi

if "${DISTCLEAN}"; then _distclean; exit "$?"; fi
if "${CLEAN}"; then _clean; exit "$?"; fi

[ -z "${DEVICE}" ] && _help 1
[[ "${OPTEE_STORAGE}" =~ ^(ree|rpmb|ree\+rpmb)$ ]] || _error "Invalid OP-TEE storage '${OPTEE_STORAGE}'"
if ${RPMB_PROVISION} && [ "${OPTEE_STORAGE}" == "ree" ]; then
    _error "--rpmb-provision requires an RPMB --optee-storage"
fi
if ${FTPM} || ${RPMB_PROVISION}; then
    OPTEE_ENABLE=true
fi
if ${FTPM} || ${RPMB_PROVISION} || [ "${OPTEE_STORAGE}" != "ree" ]; then
    if ${OPTEE_SET} && [ "${OPTEE}" != "upstream" ]; then
        _error "--ftpm, --rpmb-provision and RPMB storage require --optee upstream"
    fi
    OPTEE=upstream
fi
[[ "${OPTEE}" == "vendor" || "${OPTEE}" == "upstream" ]] || _error "Invalid OP-TEE source '${OPTEE}'"
if ${OPTEE_ENABLE} && ! ${OPEN_TFA}; then
    _error "--optee-enable requires the open-source TF-A"
fi
if ${OPTEE_ENABLE} && [ "${OPTEE}" == "vendor" ]; then
    echo "WARNING: The vendor OP-TEE relies on Rockchip BL31 extensions and panics open TF-A." >&2
fi
if ${RPMB_PROVISION}; then
    echo "WARNING: This image permanently programs the eMMC RPMB key. Boot it once, then flash a normal build." >&2
fi
[ -f "configs/${DEVICE}.conf" ] || [ "${DEVICE}" == "all" ] || _error "Device configuration not found"

#
# Get machine architecture
#
MACHINE_TYPE=$(uname -m)

# Fix-up possible differences in reported arch
if [ ${MACHINE_TYPE} == 'arm64' ]; then
    MACHINE_TYPE='aarch64'
elif [ ${MACHINE_TYPE} == 'amd64' ]; then
    MACHINE_TYPE='x86_64'
fi

if [ ${MACHINE_TYPE} != 'aarch64' ]; then
    export CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
fi

GIT_COMMIT="$(git describe --tags --always)" || GIT_COMMIT="unknown"

# Were we an untagged commit? If so, use date to generate version (we are dirty)
if [ "$GIT_COMMIT" == "$(git rev-parse --short HEAD)" ]; then
    GIT_COMMIT="edk2-rk3588-$(date +%Y%m%d)-${GIT_COMMIT}"
fi

export WORKSPACE="${OUTDIR}/workspace"
[ -d "${WORKSPACE}" ] || mkdir "${WORKSPACE}"

ROOTDIR="$(realpath "$(dirname "$0")")"
cd "${ROOTDIR}" || exit 1

# Exit on first error
set -e

if [ "${DEVICE}" == "all" ]
then
    for i in configs/*.conf; do
        DEV="$(basename "$i" .conf)"
        if [ "${DEV}" != "RK3588" ]
        then
            echo "Building ${DEV}"
            _build "${DEV}"
        fi
    done
else
    _build "${DEVICE}"
fi
