#!/bin/bash -ex

# "apex" type build targets to build.
APEX_TARGETS=(
  system/apex/tests/testdata/sharedlibs/build/com.android.apex.test.bar:com.android.apex.test.bar
  system/apex/tests/testdata/sharedlibs/build/com.android.apex.test.foo:com.android.apex.test.foo
)

# "genrule" type build targets to build, and directory they are built from.
GENRULE_TARGETS=(
  system/apex/tests/testdata/sharedlibs/build/com.android.apex.test.bar:com.android.apex.test.bar_stripped
  system/apex/tests/testdata/sharedlibs/build/com.android.apex.test.foo:com.android.apex.test.foo_stripped
  system/apex/tests/testdata/sharedlibs/build/com.android.apex.test.sharedlibs:com.android.apex.test.sharedlibs_generated
)

if [ ! -e "build/make/core/Makefile" ]; then
    echo "$0 must be run from the top of the tree"
    exit 1
fi

OUT_DIR=$(source build/envsetup.sh > /dev/null; TARGET_PRODUCT= get_build_var OUT_DIR)
DIST_DIR=$(source build/envsetup.sh > /dev/null; TARGET_PRODUCT= get_build_var DIST_DIR)
TMPDIR=$(source build/envsetup.sh > /dev/null; TARGET_PRODUCT= get_build_var TMPDIR)

manifestdirs=()

genrules=()
for t in "${GENRULE_TARGETS[@]}"; do
    IFS=: read -a ar <<< "${t}"
    genrules+=( ${ar[1]} )
    manifestdirs+=( ${ar[0]})
done

apexrules=()
for t in "${APEX_TARGETS[@]}"; do
    IFS=: read -a ar <<< "${t}"
    apexrules+=( ${ar[1]} )
    manifestdirs+=( ${ar[0]})
done

manifestdirs=($(printf "%s\n" "${manifestdirs[@]}" | sort -u))

generated_artifacts=()
for apexversion in 1 2; do
    apexfingerprint="VERSION_${apexversion}"
    sed -i "s/#define FINGERPRINT .*/#define FINGERPRINT \"${apexfingerprint}\"/g" \
    system/apex/tests/testdata/sharedlibs/build/com.android.apex.test.bar/bar_test.cc \
    system/apex/tests/testdata/sharedlibs/build/com.android.apex.test.foo/foo_test.cc

    for d in "${manifestdirs[@]}"; do
        sed -i "s/  \"version\": .*/  \"version\": ${apexversion}/g" \
        ${d}/manifest.json
    done
    for libversion in X Y; do
        libfingerprint="VERSION_${libversion}"
        sed -i "s/#define FINGERPRINT .*/#define FINGERPRINT \"${libfingerprint}\"/g" \
        system/apex/tests/testdata/sharedlibs/build/sharedlibstest.cpp

        build/soong/soong_ui.bash \
            --make-mode \
            TARGET_PRODUCT=aosp_arm64 \
            "${apexrules[@]}" \
            "${genrules[@]}"

        for t in "${APEX_TARGETS[@]}"; do
            IFS=: read -a ar <<< "${t}"
            outfile=${ar[1]}.v${apexversion}.libv${libversion}.apex
            cp \
            "${OUT_DIR}"/target/product/generic_arm64/obj/ETC/"${ar[1]}"_intermediates/${ar[1]}.apex \
            system/apex/tests/testdata/sharedlibs/${outfile}
            generated_artifacts+=(system/apex/tests/testdata/sharedlibs/${outfile})
        done

        for t in "${GENRULE_TARGETS[@]}"; do
            IFS=: read -a ar <<< "${t}"
            outfile=${ar[1]}.v${apexversion}.libv${libversion}.apex
            cp "${OUT_DIR}"/soong/.intermediates/"${ar[0]}"/"${ar[1]}"/gen/"${ar[1]}".apex \
            system/apex/tests/testdata/sharedlibs/${outfile}
            generated_artifacts+=(system/apex/tests/testdata/sharedlibs/${outfile})
        done
    done
done

# Restore the default version string to avoid bogus diffs.
sed -i "s/#define FINGERPRINT .*/#define FINGERPRINT \"VERSION_XXX\"/g" \
system/apex/tests/testdata/sharedlibs/build/sharedlibstest.cpp \
system/apex/tests/testdata/sharedlibs/build/com.android.apex.test.bar/bar_test.cc \
system/apex/tests/testdata/sharedlibs/build/com.android.apex.test.foo/foo_test.cc

for d in "${manifestdirs[@]}"; do
    sed -i "s/  \"version\": .*/  \"version\": 1/g" \
    ${d}/manifest.json
done

ls -l "${generated_artifacts[@]}"
