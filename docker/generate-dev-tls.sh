#!/usr/bin/env bash
# Generate short-lived, same-container mTLS credentials for the local-CN
# benchmark profile. This is deliberately not a production certificate issuer:
# the roots stay only in a private temporary directory and all generated
# credentials disappear with the container writable layer.
set -euo pipefail

if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    echo "usage: $0 CERT_DIR" >&2
    exit 2
fi

cert_dir=$1
# The Sirius/Pixi build environment can leave OPENSSL_CONF pointing at a path
# that does not exist in the slim runtime image. Development credentials must
# always use the runtime distribution's known configuration instead.
export OPENSSL_CONF=/etc/ssl/openssl.cnf
umask 077
mkdir -p "${cert_dir}"
chmod 700 "${cert_dir}"

for cert_file in \
    sidecar-flight-ca.crt mo-flight-client-ca.crt mo-flight-client.crt mo-flight-client.key \
    mo-resolver-server-ca.crt sidecar-read-client-ca.crt mo-resolver-server.crt mo-resolver-server.key \
    sidecar-flight-server.crt sidecar-flight-server.key \
    sidecar-read-client.crt sidecar-read-client.key; do
    if [ -e "${cert_dir}/${cert_file}" ]; then
        echo "[dev-tls] refusing to overwrite ${cert_dir}/${cert_file}" >&2
        exit 1
    fi
done

work_dir=$(mktemp -d "${cert_dir}/.dev-tls.XXXXXX")
cleanup() {
    rm -rf "${work_dir}"
}
trap cleanup EXIT

generate_ca() {
    local name=$1
    local subject=$2
    openssl req -x509 -new -newkey rsa:2048 -nodes -sha256 -days 1 \
        -keyout "${work_dir}/${name}.key" \
        -out "${work_dir}/${name}.crt" \
        -subj "${subject}" \
        -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        >/dev/null 2>&1
}

generate_leaf() {
    local name=$1
    local ca_name=$2
    local subject=$3
    local usage=$4
    local dns_name=${5:-}
    local -a request_args=(
        req -new -newkey rsa:2048 -nodes -sha256
        -keyout "${work_dir}/${name}.key"
        -out "${work_dir}/${name}.csr"
        -subj "${subject}"
        -addext "basicConstraints=critical,CA:FALSE"
        -addext "keyUsage=critical,digitalSignature,keyEncipherment"
        -addext "extendedKeyUsage=${usage}"
    )
    if [ -n "${dns_name}" ]; then
        request_args+=( -addext "subjectAltName=DNS:${dns_name}" )
    fi
    openssl "${request_args[@]}" >/dev/null 2>&1
    openssl x509 -req -sha256 -days 1 -copy_extensions copy \
        -in "${work_dir}/${name}.csr" \
        -CA "${work_dir}/${ca_name}.crt" \
        -CAkey "${work_dir}/${ca_name}.key" \
        -CAcreateserial \
        -CAserial "${work_dir}/${name}.srl" \
        -out "${work_dir}/${name}.crt" \
        >/dev/null 2>&1
}

generate_ca sidecar-flight-ca "/CN=dev-sidecar-flight-ca"
generate_ca mo-flight-client-ca "/CN=dev-mo-flight-client-ca"
generate_ca mo-resolver-server-ca "/CN=dev-mo-resolver-server-ca"
generate_ca sidecar-read-client-ca "/CN=dev-sidecar-read-client-ca"

generate_leaf sidecar-flight-server sidecar-flight-ca "/CN=sidecar" serverAuth sidecar
generate_leaf mo-flight-client mo-flight-client-ca "/CN=dev-mo-flight-client" clientAuth
generate_leaf mo-resolver-server mo-resolver-server-ca "/CN=localhost" serverAuth localhost
generate_leaf sidecar-read-client sidecar-read-client-ca "/CN=dev-sidecar-read-client" clientAuth

for cert_file in \
    sidecar-flight-ca.crt mo-flight-client-ca.crt mo-flight-client.crt mo-flight-client.key \
    mo-resolver-server-ca.crt sidecar-read-client-ca.crt mo-resolver-server.crt mo-resolver-server.key \
    sidecar-flight-server.crt sidecar-flight-server.key \
    sidecar-read-client.crt sidecar-read-client.key; do
    install -m 600 "${work_dir}/${cert_file}" "${cert_dir}/${cert_file}"
done
printf '%s\n' 'mo-sirius-sidecar-dev-tls-v1' > "${cert_dir}/.dev-tls-generated"
chmod 600 "${cert_dir}/.dev-tls-generated"

echo "[dev-tls] Generated ephemeral local-CN Flight credentials in ${cert_dir}." >&2
