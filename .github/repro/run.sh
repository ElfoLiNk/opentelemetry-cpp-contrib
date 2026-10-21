set -e
dnf -q -y module enable php:7.2 >/dev/null 2>&1; dnf -q -y install php httpd mod_ssl glibc-utils procps-ng openssl binutils python3 >/dev/null 2>&1
cp -r /p/opentelemetry-webserver-sdk /opt/opentelemetry-webserver-sdk-1.29
N=/opt/opentelemetry-webserver-sdk-1.29; mkdir -p $N/logs; chmod 777 $N/logs
sed "s|__agent_log_dir__|$N/logs|g; s|/opt/opentelemetry-webserver-sdk/logs|$N/logs|g" $N/conf/opentelemetry_sdk_log4cxx.xml.template > $N/conf/opentelemetry_sdk_log4cxx.xml
grep -n "file\"" $N/conf/opentelemetry_sdk_log4cxx.xml | head -2
sed -i 's/value="info"/value="trace"/g' $N/conf/opentelemetry_sdk_log4cxx.xml
cat > /etc/httpd/conf.modules.d/99-otel.conf <<C
LoadFile $N/sdk_lib/lib/libopentelemetry_common.so
LoadFile $N/sdk_lib/lib/libopentelemetry_resources.so
LoadFile $N/sdk_lib/lib/libopentelemetry_instrumentation_scope.so
LoadFile $N/sdk_lib/lib/libopentelemetry_trace.so
LoadFile $N/sdk_lib/lib/libopentelemetry_logs.so
LoadFile $N/sdk_lib/lib/libopentelemetry_metrics.so
LoadFile $N/sdk_lib/lib/libopentelemetry_exporter_ostream_span.so
LoadFile $N/sdk_lib/lib/libopentelemetry_http_client_curl.so
LoadFile $N/sdk_lib/lib/libopentelemetry_webserver_sdk.so
LoadModule otel_apache_module $N/WebServerModule/Apache/libmod_apache_otel.so
ApacheModuleEnabled ON
ApacheModuleOtelSpanExporter otlp
ApacheModuleOtelExporterEndpoint github.com:443
ApacheModuleOtelSslEnabled ON
ApacheModuleOtelSslCertificatePath  /etc/ssl/certs/ca-bundle.crt
ApacheModuleOtelSpanProcessor Batch
ApacheModuleOtelSampler AlwaysOn
ApacheModuleOtelMaxQueueSize 1024
ApacheModuleServiceName tab-httpd
ApacheModuleServiceNamespace repro
ApacheModuleServiceInstanceId c1
ApacheModuleResolveBackends ON
ApacheModuleTraceAsError OFF
ApacheModuleWebserverContext repro tab-httpd c1
C
openssl req -x509 -newkey rsa:2048 -nodes -subj /CN=localhost -keyout /etc/pki/tls/private/localhost.key -out /etc/pki/tls/certs/localhost.crt -days 2 >/dev/null 2>&1
sed -i 's/^LoadModule mpm_event_module/#&/; s/^#LoadModule mpm_prefork_module/LoadModule mpm_prefork_module/' /etc/httpd/conf.modules.d/00-mpm.conf
httpd -V | grep MPM
cat > /etc/httpd/conf.d/zz-proxy.conf <<P
ProxyPass /api http://127.0.0.1:8081/
ProxyPassReverse /api http://127.0.0.1:8081/
P
mkdir -p /var/www/backend && echo ok > /var/www/backend/index.html && echo '<?php echo "php ok";' > /var/www/html/t.php
httpd -M 2>/dev/null | grep -E "php|proxy_http"
httpd -t
( cd /var/www/backend && python3 -m http.server 8081 >/tmp/backend.log 2>&1 & )
sleep 2
# Old upstream module (webserver/v1.1.0, opentelemetry-cpp 1.2.0) installed the way FEQ01 has it.
O=/opt/opentelemetry-webserver-sdk
curl -sSLf -o /tmp/old.tgz https://github.com/open-telemetry/opentelemetry-cpp-contrib/releases/download/webserver/v1.1.0/opentelemetry-webserver-sdk-x64-linux.tgz
tar -xzf /tmp/old.tgz -C /opt; mkdir -p $O/logs; chmod 777 $O/logs
sed "s|__agent_log_dir__|$O/logs|g" $O/conf/opentelemetry_sdk_log4cxx.xml.template > $O/conf/opentelemetry_sdk_log4cxx.xml
sed -e "s|$N/|$O/|g" -e "/instrumentation_scope\|libopentelemetry_logs\|libopentelemetry_metrics\|http_client_curl/d" /etc/httpd/conf.modules.d/99-otel.conf \
  | sed "s|^LoadFile $O/sdk_lib/lib/libopentelemetry_exporter_ostream_span.so|LoadFile $O/sdk_lib/lib/libopentelemetry_otlp_recordable.so\nLoadFile $O/sdk_lib/lib/libopentelemetry_exporter_ostream_span.so\nLoadFile $O/sdk_lib/lib/libopentelemetry_exporter_otlp_grpc.so|" > /tmp/old-otel.conf
cp /etc/httpd/conf.modules.d/99-otel.conf /tmp/new-otel.conf
traffic() { for u in / /t.php /api/ /t.php /api/; do curl -s -o /dev/null -m 10 -w "$1 curl $u=%{http_code}\n" "http://127.0.0.1$u" || echo "$1 curl $u FAILED"; done; }
segv() { echo "$1 segfaults so far: $(grep -c 'Segmentation fault' /var/log/httpd/error_log)"; }

echo "== phase 1: old module, fresh start"
cp /tmp/old-otel.conf /etc/httpd/conf.modules.d/99-otel.conf; httpd -t
/usr/sbin/httpd -k start; sleep 10; traffic old; sleep 3; segv old

echo "== phase 2: switch to new module with graceful (what was done on FEQ01)"
cp /tmp/new-otel.conf /etc/httpd/conf.modules.d/99-otel.conf; httpd -t
/usr/sbin/httpd -k graceful; sleep 10; traffic graceful; sleep 3; segv graceful
grep -E "exit signal" /var/log/httpd/error_log | tail -3

echo "== phase 3: full stop/start with new module"
/usr/sbin/httpd -k stop; sleep 5; : > /var/log/httpd/error_log
/usr/sbin/httpd -k start; sleep 10; traffic restart; sleep 3; segv restart
echo "== sdk.log (new) tail"; tail -4 $N/logs/sdk.log 2>/dev/null | sed -E "s/^[0-9-]+ [0-9:.]+ UTC //"
