#!/bin/sh

datadir=$1
bindir=$2
pkgconfdir=$3
pkglibdir=$4
localstatedir=$5

[ -n "$DESTDIR" ] && DESTDIR=${DESTDIR%%/}/

mv "${DESTDIR}${datadir}/bash-completion/completions/nmcli-completion" \
   "${DESTDIR}${datadir}/bash-completion/completions/nmcli"

for alias in nmtui-connect nmtui-edit nmtui-hostname; do
    ln -sf nmtui "${DESTDIR}${bindir}/$alias"
done

for dir in "${pkgconfdir}/conf.d" \
           "${pkgconfdir}/system-connections" \
           "${pkgconfdir}/dispatcher.d/no-wait.d" \
           "${pkgconfdir}/dispatcher.d/pre-down.d" \
           "${pkgconfdir}/dispatcher.d/pre-up.d" \
           "${pkgconfdir}/dnsmasq.d" \
           "${pkgconfdir}/dnsmasq-shared.d" \
           "${pkgconfdir}/conf.d" \
           "${pkgconfdir}/VPN" \
           "${localstatedir}/lib/NetworkManager"; do
    mkdir -p "${DESTDIR}${dir}"
done

if [ "$6" = install_docs ]; then
    mandir=$7

    for alias in nmtui-connect nmtui-edit nmtui-hostname; do
        ln -f nmtui.1 "${DESTDIR}${mandir}/man1/${alias}.1"
    done

    ln -f NetworkManager.conf.5 "${DESTDIR}${mandir}/man5/nm-system-settings.conf"
fi

