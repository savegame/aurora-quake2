%define _app_orgname ru.sashikknox
%define _app_appname QuakeII
%define _app_launcher_name Quake 2

Name:       %{_app_orgname}.%{_app_appname}
Summary:    Quake 2 for Aurora OS
Release:    2
Version:    1.0.0
Group:      Amusements/Games
License:    GPL-2.0+
Source0:    %{name}.tar.gz

%define __requires_exclude ^libopenal.*\.so.*|libvorbis.*\.so.*|libogg.*\.so.*|libSDL2.*\.so.*|libz.*\.so.*$
%define __provides_exclude_from ^%{_datadir}/%{name}/lib/.*\.so.*$

BuildRequires: cmake
BuildRequires: ninja
BuildRequires: patchelf
BuildRequires: pkgconfig(wayland-client)
BuildRequires: pkgconfig(wayland-cursor)
BuildRequires: pkgconfig(wayland-egl)
BuildRequires: pkgconfig(wayland-protocols)
BuildRequires: pkgconfig(wayland-scanner)
BuildRequires: pkgconfig(glesv2)
BuildRequires: pkgconfig(glesv1_cm)
BuildRequires: pkgconfig(xkbcommon)
BuildRequires: pkgconfig(vulkan)
BuildRequires: pkgconfig(egl)
BuildRequires: pkgconfig(libpulse)
BuildRequires: pkgconfig(dbus-1)
BuildRequires: pkgconfig(libudev)

%description
Quake 2 ported to Aurora OS using SDL2 and OpenGL ES 2 backend.

%prep
%setup -q -n %{name}-%{version}

%build
# В кросс-сборке под Авророй не запускаем тестовые бинарники таргета.
cmake \
    -G Ninja \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_WORKS=1 \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=%{_arch} \
    -DAURORA_ORG=%{_app_orgname} \
    -DAURORA_APP=%{_app_appname} \
    -DAURORA_FBO=ON \
    -DBUNDLE_SDL2=ON \
    -S . \
    -B build/%{_arch}/rpm

cmake --build build/%{_arch}/rpm

%install
install -m 0755 -D build/%{_arch}/rpm/QuakeII %{buildroot}%{_bindir}/%{name}
patchelf --force-rpath --set-rpath %{_datadir}/%{name}/lib %{buildroot}%{_bindir}/%{name}

# Bundled SDL2 (Aurora fixes, savegame/SDL)
install -d %{buildroot}%{_datadir}/%{name}/lib
install -D -s build/%{_arch}/rpm/libsdl/libSDL2-2.0.so* -t %{buildroot}%{_datadir}/%{name}/lib

# Mission packs (game.so)
install -m 0755 -D build/%{_arch}/rpm/baseq2/game.so   %{buildroot}%{_datadir}/%{name}/baseq2/game.so
install -m 0755 -D build/%{_arch}/rpm/ctf/game.so     %{buildroot}%{_datadir}/%{name}/ctf/game.so
install -m 0755 -D build/%{_arch}/rpm/rogue/game.so    %{buildroot}%{_datadir}/%{name}/rogue/game.so
install -m 0755 -D build/%{_arch}/rpm/xatrix/game.so  %{buildroot}%{_datadir}/%{name}/xatrix/game.so

# Directories for user-provided .pak data files
install -d %{buildroot}%{_datadir}/%{name}/baseq2
install -d %{buildroot}%{_datadir}/%{name}/ctf
install -d %{buildroot}%{_datadir}/%{name}/rogue
install -d %{buildroot}%{_datadir}/%{name}/xatrix

# Gamepad mappings database (SDL_GameControllerDB), read at gamepad init
install -m 644 -D gamecontrollerdb.txt %{buildroot}%{_datadir}/%{name}/gamecontrollerdb.txt

# Icons
install -m 644 -D icons/86.png  %{buildroot}%{_datadir}/icons/hicolor/86x86/apps/%{name}.png
install -m 644 -D icons/108.png %{buildroot}%{_datadir}/icons/hicolor/108x108/apps/%{name}.png
install -m 644 -D icons/128.png %{buildroot}%{_datadir}/icons/hicolor/128x128/apps/%{name}.png
install -m 644 -D icons/172.png %{buildroot}%{_datadir}/icons/hicolor/172x172/apps/%{name}.png

# Desktop file
sed "s/__ORGNAME__/%{_app_orgname}/g" game.desktop.in > %{name}.desktop
sed -i "s/__APPNAME__/%{_app_appname}/g" %{name}.desktop
sed -i "s/__LAUNCHER_NAME__/%{_app_launcher_name}/g" %{name}.desktop

install -m 644 -D %{name}.desktop %{buildroot}%{_datadir}/applications/%{name}.desktop

%files
%defattr(-,root,root,-)
%attr(755,root,root) %{_bindir}/%{name}
%{_datadir}/icons/hicolor/86x86/apps/%{name}.png
%{_datadir}/icons/hicolor/108x108/apps/%{name}.png
%{_datadir}/icons/hicolor/128x128/apps/%{name}.png
%{_datadir}/icons/hicolor/172x172/apps/%{name}.png
%{_datadir}/applications/%{name}.desktop
%{_datadir}/%{name}
