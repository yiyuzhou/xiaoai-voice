"""SSH discovery, installation and reversible disable, confined to /data/xiaoai-plus."""
import argparse
import concurrent.futures
import configparser
from datetime import datetime
import io
import ipaddress
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import stat
import subprocess
import sys
import tarfile
import time
from firmware import LOCAL, ROOT, download, sha, StatusParser

PACKAGE_SHA = '34629b02148219eed88eb1662b50c13ff7c221b176a3fb0231eec38c770704d5'
PACKAGE_SIZE = 10287484
PACKAGE_URL = 'https://github.com/rolazd/xiaoai-doubao/releases/download/v0.1.0/xiaoai-doubao-oh2p-v0.1.0.tar.gz'
MODEL_URL = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2'
MODEL_SHA = '68447f4fbc67e70eee3a93961f36e81e98f47aef73ce7e7ca00885c6cd3616a6'
CA_URL = 'https://curl.se/ca/cacert-2026-11-05.pem'
CA_SHA = '427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285'
APP='/data/xiaoai-plus'

def run(cmd, data=None, timeout=30):
    p=subprocess.run(list(map(str,cmd)),input=data,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout)
    if p.returncode: raise RuntimeError('command failed: '+str(cmd[0])+' (details suppressed to protect credentials)')
    return p.stdout.decode(errors='replace')

def ssh(a,cmd,data=None,timeout=30):
    return run(['ssh','-p',str(a.port),'-i',str(a.key.expanduser()),'-o','BatchMode=yes','-o','ConnectTimeout=10','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(a.known_hosts),'-o','HostKeyAlgorithms=+ssh-rsa','-o','PubkeyAcceptedAlgorithms=+ssh-rsa','root@'+a.host,cmd],data,timeout)

def discovery(a):
    if not a.host:
        if not a.mac or not re.fullmatch(r'(?:[0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}',a.mac): raise RuntimeError('请从米家App抄写MAC参数')
        route=run(['route','-n','get','default']);iface=re.search(r'interface:\s*(\S+)',route).group(1)
        ip=run(['ipconfig','getifaddr',iface]).strip();mask=run(['ipconfig','getoption',iface,'subnet_mask']).strip()
        network=ipaddress.ip_network(ip+'/'+mask,strict=False)
        def probe(host):
            try:
                with socket.create_connection((str(host),22),timeout=.25): pass
            except OSError: pass
        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:list(pool.map(probe,network.hosts()))
        arp=run(['arp','-an']);wanted=a.mac.lower().replace('-',':')
        matches=[]
        for line in arp.splitlines():
            m=re.search(r'\(([^)]+)\) at ([0-9a-f:]+)',line)
            if m and ':'.join(x.zfill(2) for x in m[2].split(':'))==wanted:matches.append(m[1])
        if len(matches)!=1: raise RuntimeError('MAC没有唯一IP；确认同Wi-Fi、非蓝牙模式')
        a.host=matches[0]
    scan=run(['ssh-keyscan','-t','rsa','-p',str(a.port),a.host]).encode()
    if not scan.strip(): raise RuntimeError('SSH host key missing')
    host=a.host if a.port==22 else '['+a.host+']:'+str(a.port)
    if a.known_hosts.exists():
        old=run(['ssh-keygen','-F',host,'-f',a.known_hosts])
        oldkeys={tuple(x.split()[1:3]) for x in old.splitlines() if x and not x.startswith('#')}
        newkeys={tuple(x.split()[1:3]) for x in scan.decode().splitlines() if x and not x.startswith('#')}
        if not oldkeys or not oldkeys.intersection(newkeys) or any(ot==nt and ok!=nk for ot,ok in oldkeys for nt,nk in newkeys): raise RuntimeError('主机指纹与记录不符')
    else:
        a.known_hosts.write_bytes(scan);a.known_hosts.chmod(0o600)
    print(run(['ssh-keygen','-lf',a.known_hosts]).strip())
    print(precheck(a,0))
    p=LOCAL/'speaker.json';p.write_text(json.dumps({'host':a.host,'port':a.port}));p.chmod(0o600)

def precheck(a, size):
    text=ssh(a,"cat /usr/share/mico/version; cat /proc/mtd; df -Pk /data; if [ -d /data/xiaoai-plus ]; then echo EXISTING_INSTALL; else echo NEW_INSTALL; fi")
    if not re.search(r'\bOH2P\b',text) or not re.search(r'\b1\.62\.2\b',text): raise RuntimeError('仅支持OH2P 1.62.2')
    df=ssh(a,"df -Pk /data | tail -1").split()
    if int(df[3])*1024<=size*2: raise RuntimeError('/data剩余空间必须大于安装包两倍')
    return text

def config(a):
    key=LOCAL/'volc_key'
    if stat.S_IMODE(key.stat().st_mode)!=0o600: raise RuntimeError('.local/volc_key必须0600')
    value=key.read_text().strip()
    if not value or '\n' in value: raise RuntimeError('invalid key file')
    cfg=configparser.ConfigParser(interpolation=None);cfg.read(ROOT/'xiaoai-plus/config.ini.example')
    cfg['realtime']['api_key']=value
    if a.child: cfg['memory']['child_names']=', '.join(a.child)
    cfg['memory']['enabled']='off'
    if a.memory_server:
        secret=a.device_secret_file
        if secret is None or stat.S_IMODE(secret.stat().st_mode)!=0o600: raise RuntimeError('记忆设备密钥文件须0600')
        value=secret.read_text().strip()
        if not re.fullmatch('[0-9a-fA-F]{64}',value): raise RuntimeError('设备密钥须64位hex')
        cfg['memory'].update({'enabled':'on','server_url':a.memory_server,'device_secret':value})
    p=LOCAL/'config.ini'
    with p.open('w') as f:cfg.write(f)
    p.chmod(0o600);return p

def content(a,package):
    stage=LOCAL/'install';stage.mkdir(exist_ok=True,mode=0o700)
    with tarfile.open(package) as t:
        for m in t.getmembers():
            rel=Path(m.name)
            if rel.parts[0]!='oh2p' or '..' in rel.parts or m.issym() or m.islnk(): raise RuntimeError('unsafe package entry')
            if not m.isfile():continue
            p=stage.joinpath(*rel.parts[1:]);p.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
            p.write_bytes(t.extractfile(m).read());p.chmod(0o600)
    prepare_assets(stage)
    shutil.copyfile(config(a),stage/'config.ini');(stage/'config.ini').chmod(0o600)
    return stage

def prepare_assets(stage):
    LOCAL.mkdir(exist_ok=True, mode=0o700)
    model=LOCAL/'kws.tar.bz2';download(MODEL_URL,model,MODEL_SHA)
    assets=stage/'assets';assets.mkdir(exist_ok=True,mode=0o700)
    with tarfile.open(model) as t:
        for out,name in [('encoder.onnx','encoder-epoch-13-avg-2-chunk-8-left-64.int8.onnx'),('decoder.onnx','decoder-epoch-13-avg-2-chunk-8-left-64.onnx'),('joiner.onnx','joiner-epoch-13-avg-2-chunk-8-left-64.int8.onnx'),('tokens.txt','tokens.txt'),('en.phone','en.phone')]:
            m=t.getmember('sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20/'+name)
            (assets/out).write_bytes(t.extractfile(m).read());(assets/out).chmod(0o600)
    ca=LOCAL/'ca-certificates.crt'
    download(CA_URL,ca,CA_SHA)
    shutil.copyfile(ca,assets/'ca-certificates.crt')
    (assets/'ca-certificates.crt').chmod(0o600)

def log_scope(offset):
    log=APP+'/xiaoai_plus.log'
    return ('if [ -f '+log+' ]; then if [ "$(wc -c < '+log+')" -lt '+str(offset)+
            ' ]; then cat '+log+'; else tail -c +'+str(offset+1)+' '+log+'; fi; fi')

def wait_ready(a, offset):
    end=time.monotonic()+35
    while time.monotonic()<end:
        text=ssh(a,log_scope(offset))
        if 'app components started' in text and 'kws keywords: 1 wake, 6 exit' in text:
            return
        time.sleep(1)
    raise RuntimeError('启动35秒内未出现两条就绪日志')

def install(a):
    package=a.package or LOCAL/'xiaoai-doubao-oh2p-v0.1.0.tar.gz'
    if a.check_only:
        size=package.stat().st_size if package.exists() else PACKAGE_SIZE
        print(precheck(a,size));return
    if not a.package:download(PACKAGE_URL,package,PACKAGE_SHA)
    if sha(package)!=PACKAGE_SHA: raise RuntimeError('package SHA-256 mismatch')
    print(precheck(a,package.stat().st_size))
    stage=content(a,package)
    stamp=datetime.now().strftime('%Y%m%d-%H%M%S')
    # Copy only the items this installer replaces; never recurse into earlier rollbacks.
    ssh(a,"set -eu; umask 077; if [ -d "+APP+" ]; then mkdir "+APP+'/rollback-'+stamp+"; for f in xiaoai_plus_speaker assets keywords config.ini config.ini.example boot.sh LICENSE; do if [ -e "+APP+'/"$f" ]; then cp -pR '+APP+'/"$f" '+APP+'/rollback-'+stamp+'/; fi; done; else mkdir '+APP+'; fi; chmod 700 '+APP)
    ssh(a,"if [ -f "+APP+"/guardian.pid ]; then p=$(cat "+APP+"/guardian.pid); kill \"$p\" 2>/dev/null || true; fi; p=$(pidof xiaoai_plus_speaker || true); if [ -n \"$p\" ]; then kill $p; fi")
    for p in sorted(stage.rglob('*')):
        if not p.is_file():continue
        target=APP+'/'+p.relative_to(stage).as_posix()
        mode='700' if p.name in ('xiaoai_plus_speaker','boot.sh') else '600'
        ssh(a,'set -eu; umask 077; mkdir -p '+shlex.quote(str(Path(target).parent))+'; cat > '+shlex.quote(target)+'; chmod '+mode+' '+shlex.quote(target),p.read_bytes())
    if a.skip_arm_checks:
        print('SIMULATION: skipped ARM check-config, startup, process/log/native/CPU checks');return
    ssh(a,'cd '+APP+'; ./xiaoai_plus_speaker --check-config -c config.ini >/dev/null')
    offset=int(ssh(a,'if [ -f '+APP+'/xiaoai_plus.log ]; then wc -c < '+APP+'/xiaoai_plus.log; else echo 0; fi').strip())
    ssh(a,'/bin/sh '+APP+'/boot.sh </dev/null >/dev/null',timeout=90)
    wait_ready(a,offset)
    result=ssh(a,"set -eu; test -f "+APP+"/xiaoai_plus.log; pidof xiaoai_plus_speaker; pidof mipns-xiaomi; if { "+log_scope(offset)+"; } | grep -Ei 'error|fatal' >/dev/null; then exit 1; fi")
    if len(result.splitlines())<2: raise RuntimeError('speaker/native process missing')
    # Process ticks over wall time measure a full 3-minute idle window; SSH only reads /proc.
    def ticks():
        return [int(x) for x in ssh(a,"pid=$(pidof xiaoai_plus_speaker); set -- $(cat /proc/$pid/stat); echo ${14} ${15}; getconf CLK_TCK 2>/dev/null || echo 100").split()]
    first=ticks();start=time.monotonic();print('保持安静，测量空闲CPU 3分钟。',flush=True)
    for _ in range(18):time.sleep(10);print('.',end='',flush=True)
    last=ticks();avg=100*(last[0]+last[1]-first[0]-first[1])/first[2]/(time.monotonic()-start)
    print('\nidle_cpu_average_percent=%.1f (作者约80%%)'%avg)

def uninstall(a):
    ssh(a,"set -eu; if [ -f "+APP+"/guardian.pid ]; then p=$(cat "+APP+"/guardian.pid); kill \"$p\"; fi; p=$(pidof xiaoai_plus_speaker || true); if [ -n \"$p\" ]; then kill $p; fi; if [ -f "+APP+"/boot.sh ]; then mv "+APP+"/boot.sh "+APP+"/boot.sh.disabled-"+datetime.now().strftime('%Y%m%d-%H%M%S')+"; fi")
    if a.skip_arm_checks: print('SIMULATION: skipped reboot/native process verification');return
    try:ssh(a,'reboot')
    except RuntimeError: pass # SSH disconnect is expected during reboot.
    time.sleep(10)
    for _ in range(12):
        try:
            ssh(a,'! pidof xiaoai_plus_speaker && pidof mipns-xiaomi');return
        except (RuntimeError,subprocess.TimeoutExpired):time.sleep(5)
    raise RuntimeError('重启后未确认原厂进程；请检查米家App')

def entry():
    os.umask(0o077);step=int(sys.argv.pop(1))
    p=StatusParser(description={8:'按米家App MAC寻找同网段音箱，保存SSH指纹并只读检查；首次请核对打印指纹。',9:'安装到/data/xiaoai-plus；先在Mac创建0600的.local/volc_key；--execute才写入，--check-only只读。',10:'停豆包、改名boot.sh并重启回原厂小爱；保留数据；--execute才执行。'}[step],epilog='OK: help complete\nNEXT: 按本步说明操作。',formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('mac',nargs='?',help='米家App中的MAC，08必填；--host仅用于已知地址或本地Docker')
    p.add_argument('--host',help='指定SSH地址，Docker测试用127.0.0.1')
    p.add_argument('--port',type=int,default=22)
    p.add_argument('--key',type=Path,default=Path('~/.ssh/xiaoai_doubao_rsa'))
    p.add_argument('--known-hosts',type=Path,default=LOCAL/'known_hosts')
    p.add_argument('--package',type=Path,help='本地v0.1.0安装包')
    p.add_argument('--child',action='append',help='孩子名字，可重复 --child Amy --child Ben')
    p.add_argument('--memory-server',help='可选记忆后台URL')
    p.add_argument('--device-secret-file',type=Path,help='可选记忆后台设备密钥文件(0600)')
    p.add_argument('--check-only',action='store_true',help='只读装前检查，不生成配置、不下载、不写设备')
    p.add_argument('--execute',action='store_true',help='允许安装或卸载；默认仅打印命令')
    p.add_argument('--skip-arm-checks',action='store_true',help='仅本地Docker模拟：跳过ARM执行、进程/日志/CPU和重启验收')
    a=p.parse_args()
    if step!=8 and not a.execute and not a.check_only:
        print('ssh: '+('备份/data/xiaoai-plus；拷贝程序和配置；--check-config；启动boot.sh' if step==9 else 'kill豆包；mv boot.sh；reboot'))
        print('OK: dry run; no SSH commands\nNEXT: 核对参数后添加 --execute');return
    try:
        if step!=8 and not a.host:
            saved=json.loads((LOCAL/'speaker.json').read_text());a.host=saved['host'];a.port=saved['port']
        if not a.check_only:LOCAL.mkdir(mode=0o700,exist_ok=True)
        if step==8:discovery(a)
        elif step==9:install(a)
        else:uninstall(a)
        print('OK: '+('装前检查完成' if a.check_only else str(step)+' complete'))
        print('NEXT: '+{8:'开通火山并创建.local/volc_key，再运行 bash setup/09-install.sh --execute',9:'对音箱说“豆包豆包”' if not a.check_only else '检查通过；需要安装时添加 --execute',10:'试喊“小爱同学”，在米家App确认正常'}[step])
    except Exception as e:
        # Never render exception payloads that could contain generated configuration.
        reason=str(e) if isinstance(e,RuntimeError) else type(e).__name__
        print('STOP: '+reason+'\nNEXT: 检查原因；主机指纹不符时停止并核对设备');sys.exit(1)

if __name__=='__main__':
    if len(sys.argv)==1 or sys.argv[1] in ('-h','--help'):
        print('Internal SSH helper: use 08/09/10 scripts with --help.\nOK: help complete\nNEXT: Run setup/08-find-speaker.sh --help')
    else: entry()
