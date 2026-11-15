🚀 **v0.3.3 is out** (PC: full release · Android: pre-release)

⚡ **A lot faster on weak machines and phones**
I measured dozens of speed-ups in my renderer on a weak laptop and kept only the ones that clearly paid. They're now on for everyone. On my Surface Pro 6 (Intel UHD 620), warmed up, against v0.3.2:
• Daniel Bryan's entrance: 38 → 46 fps (+21 %)
• Brock Lesnar's entrance: 41 → 51 fps (+26 %)
• Matches hold 60 more steadily
Pictures should look exactly the same. If anything looks off that didn't in v0.3.2, start with `--wwe2k14_s102_bundle3=false` and let me know.

📱 **Black title screen on some phones: fixed**
Phones whose graphics chip can't read the game's compressed textures (Mali, Samsung Xclipse, older Adreno 6xx) got a black screen from the title on. Those textures are now unpacked when they load. I could only test it on a simulated Mali, so if you had the black screen, please try it and tell me!

🛠️ **Also fixed**
• A picture could stay a frame old after a skipped frame
• A crowd drawing step read past the end of a table

📝 Full notes on the release page. Reports from the launcher (Report a problem) help a lot, especially from Mali / Xclipse phones and weak laptops.
