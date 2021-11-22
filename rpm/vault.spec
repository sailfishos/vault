Summary: Incremental backup/restore framework
Name: vault
Version: 1.0.2
Release: 1
License: LGPLv2
URL: https://github.com/sailfishos/vault
Source0: %{name}-%{version}.tar.bz2
BuildRequires: pkgconfig(Qt5Core) >= 5.2.0
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
Incremental backup/restore framework

%package devel
Summary: vault headers etc.
Requires: %{name} = %{version}-%{release}
%description devel
vault library header files etc.

%prep
%setup -q -n %{name}-%{version}

%build
%qmake5 "VERSION=%{version}"
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
%qmake5_install

%files
%defattr(-,root,root,-)
%license LICENSE
%{_libdir}/libvault.so*

%files devel
%defattr(-,root,root,-)
%{_libdir}/pkgconfig/vault.pc
%dir %{_includedir}/vault
%{_includedir}/vault/*.h

%post
/sbin/ldconfig || :

%postun
/sbin/ldconfig || :
