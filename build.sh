../meson-1.10.0/meson.py setup --cross-file build-win64-sys.txt --native-file build-osx.txt -Dwine_builtin_dll=true -Dnative_llvm_path=toolchains/llvm-darwin -Dwine_install_path=toolchains/wine -Denable_nvapi=true -Denable_nvngx=true build --buildtype release --prefix $(pwd)/dxmtBuild --strip --reconfigure
../meson-1.10.0/meson.py compile -C build
mv build/src/d3d10/d3d10core.dll /Applications/CrossOver\ copy.app/Contents/SharedSupport/CrossOver/lib/dxmt/x86_64-windows/d3d10core.dll
mv build/src/d3d11/d3d11.dll /Applications/CrossOver\ copy.app/Contents/SharedSupport/CrossOver/lib/dxmt/x86_64-windows/d3d11.dll
mv build/src/dxgi/dxgi.dll /Applications/CrossOver\ copy.app/Contents/SharedSupport/CrossOver/lib/dxmt/x86_64-windows/dxgi.dll
mv build/src/nvapi/nvapi64.dll /Applications/CrossOver\ copy.app/Contents/SharedSupport/CrossOver/lib/dxmt/x86_64-windows/nvapi64.dll
mv build/src/nvngx/nvngx.dll /Applications/CrossOver\ copy.app/Contents/SharedSupport/CrossOver/lib/dxmt/x86_64-windows/nvngx.dll
mv build/src/winemetal/winemetal.dll /Applications/CrossOver\ copy.app/Contents/SharedSupport/CrossOver/lib/dxmt/x86_64-windows/winemetal.dll
mv build/src/winemetal/unix/winemetal.so /Applications/CrossOver\ copy.app/Contents/SharedSupport/CrossOver/lib/dxmt/x86_64-unix/winemetal.so