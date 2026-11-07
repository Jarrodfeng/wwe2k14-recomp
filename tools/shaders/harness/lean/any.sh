# usage: any.sh shader   (head, skin, ...)
s=$1
W=/c/Users/Jarrod/wwe2k14-recomp/out/wt/s95-wr
GL=$(ls /c/VulkanSDK/*/Bin/glslangValidator.exe | tail -1)
V=/c/Users/Jarrod/wwe2k14-recomp/out/restore_check_epi/vkrun.exe
py -I pp.py $W/src/scene/wrestler_$s.frag ${s}_plain.glsl
py -I pp.py $W/src/scene/wrestler_$s.frag ${s}_lean.glsl LEAN
py -I mkh_n.py x ${s}_plain.glsl
NP=$(py -I gen_any.py $s | cut -d' ' -f2)
for v in plain lean; do
 py -I mkharness_n.py mine ${s}_$v.glsl ${s}_$v.comp >/dev/null
 sed -i 's|return 0.5 + 0.5 \* sin(a \* p.x + b \* p.y + ph + 0.4 \* sin(p.x \* 5.0 + s));|return (0.5 + 0.5 * sin(a * p.x + b * p.y + ph + 0.4 * sin(p.x * 5.0 + s))) * step(0.35, fract(p.x * 13.0 + s * 3.0 + p.y * 7.0));|' ${s}_$v.comp
 "$GL" -V --target-env vulkan1.2 -S comp ${s}_$v.comp -o ${s}_$v.spv | grep -i error
 $V ${s}_$v.spv ${s}_in.bin ${s}_out_$v.bin 65536 $NP >/dev/null
done
py -I cmp4.py $s
