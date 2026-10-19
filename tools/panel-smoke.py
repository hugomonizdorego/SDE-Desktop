#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native panel integration checks with isolated QA D-Bus/audio/network fixtures."""
import argparse
import os
from pathlib import Path
import re
import subprocess as sp
import tempfile
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--bin-dir',type=Path,default=Path('/usr/bin'))
p.add_argument('--test-dir',type=Path,required=True)
p.add_argument('--output-dir',type=Path,default=Path('dist/panel-smoke'))
p.add_argument('--size',default='1366x768')
p.add_argument('--scale',type=int,default=100)
p.add_argument('--tcp-display',action='store_true')
p.add_argument('--tcp-bus',action='store_true',help='Private localhost bus for containers without Unix sockets')
a=p.parse_args();W,H=map(int,a.size.split('x'));scale=a.scale/100
out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
tmp=tempfile.TemporaryDirectory(prefix='sde-panel-qa-');home=Path(tmp.name);(home/'bin').mkdir();(home/'.config/sde').mkdir(parents=True)
e=os.environ.copy();e.update(HOME=str(home),XDG_CONFIG_HOME=str(home/'.config'),XDG_CONFIG_DIRS=str(home/'system'),DISPLAY='127.0.0.1:96' if a.tcp_display else ':96',LANG='C.UTF-8',LC_ALL='C.UTF-8',PATH=str(home/'bin')+':'+str(a.bin_dir.resolve())+':'+e['PATH'])
processes=[]
def spawn(*cmd):
    log=open(home/(Path(cmd[0]).name+'.log'),'w');proc=sp.Popen(cmd,env=e,stdout=log,stderr=sp.STDOUT);processes.append(proc);return proc
def run(*cmd,check=True):return sp.run(list(map(str,cmd)),env=e,check=check,stdout=sp.PIPE,stderr=sp.PIPE,text=True)
def waitfor(fn,why):
    for _ in range(100):
        value=fn()
        if value:return value
        time.sleep(.08)
    print('Processes', [(proc.args,proc.poll()) for proc in processes],flush=True)
    print(run('xdotool','search','--name','.',check=False).stdout,flush=True)
    raise AssertionError(why)
def find(name,visible=True):
    def search():
        args=['xdotool','search']+(['--onlyvisible'] if visible else [])+['--name',name];r=run(*args,check=False)
        return r.stdout.splitlines()[-1] if r.returncode==0 and r.stdout.strip() else None
    return waitfor(search,'window not found: '+name)
def geom(w):
    text=run('xwininfo','-id',w).stdout
    return {k:int(re.search(re.escape(label)+r'\s*:\s*(-?\d+)',text).group(1)) for k,label in [('x','Absolute upper-left X'),('y','Absolute upper-left Y'),('w','Width'),('h','Height')]}
def parent(w):return int(re.search(r'Parent window id:\s*(0x[0-9a-f]+)',run('xwininfo','-tree','-id',w).stdout).group(1),16)
def click(w,x,y,button=1):
    g=geom(w);run('xdotool','mousemove',g['x']+round(x*scale),g['y']+round(y*scale),'click',button);time.sleep(.4)
def shot(name):
    time.sleep(.3);run('import','-window','root',out/(name+'.png'));print('saved',name,flush=True)
def config(style='full',top=False,moved=True,theme='light'):
    (home/'.config/sde/sde.conf').write_text(f'[appearance]\nscale={a.scale}\ntheme={theme}\n[panel]\nstyle={style}\nposition={"top" if top else "bottom"}\n[action_center]\nmove_status={str(moved).lower()}\n')
def reload():
    run('xprop','-root','-f','_SDE_CONFIG_SERIAL','32c','-set','_SDE_CONFIG_SERIAL',time.time_ns()%1000000000);time.sleep(.6)
def bounds(ac,panel,top=False):
    g=geom(ac);pg=geom(panel)
    assert g['x']+g['w']==pg['x']+pg['w'],(g,pg)
    assert g['y']==pg['y']+pg['h'] if top else g['y']+g['h']==pg['y'],(g,pg)
    assert g['x']>=0 and g['y']>=0 and g['x']+g['w']<=W and g['y']+g['h']<=H,(g,pg)
def open_section(name):
    run('sde-panel','--section',name);w=find('^Action Center$');time.sleep(.8);return w
def below_tray(ac,offset,x=190):
    for _ in range(30):
        hg=geom(str(host));g=geom(ac)
        target=hg['y']+hg['h']+round((30+offset)*scale)
        if g['y']+round(12*scale)<target<g['y']+g['h']-round(65*scale):
            run('xdotool','mousemove',g['x']+round(x*scale),target,'click','1');time.sleep(.5);return
        direction='5' if target>=g['y']+g['h']-round(65*scale) else '4'
        run('xdotool','mousemove',g['x']+g['w']-round(7*scale),g['y']+g['h']//2,'click',direction);time.sleep(.15)
    print('REVEAL',g,hg,target,offset,flush=True)
    shot('debug-reveal')
    raise AssertionError('Could not reveal action below the tray')
def log_has(text):return text in (home/'service.log').read_text()
def mode(name):
    run('dbus-send','--session','--type=method_call','--print-reply','--dest=org.bluez','/SDETest','org.sde.Test.Mode','string:'+name);time.sleep(.8)
(home/'bin/nmcli').write_text('''#!/usr/bin/env python3
import sys,os
from pathlib import Path
a=sys.argv[1:];s=' '.join(a)
with open(Path(os.environ['HOME'])/'commands.log','a') as f:f.write('NM '+s+'\\n')
if 'DEVICE,TYPE,STATE,CONNECTION' in s:print('eth0:ethernet:connected:QA Wired LAN\\nwlan0:wifi:connected:QA Design Lab')
elif 'TYPE,STATE,CONNECTION' in s:print('ethernet:connected:QA Wired LAN\\nwifi:connected:QA Design Lab')
elif a==['radio','wifi']:print('enabled')
elif a==['radio','wwan']:print('disabled')
elif 'IN-USE,SIGNAL,SECURITY,SSID' in s:print('*:84:WPA2:QA Design Lab\\n:62:WPA2:QA Library\\n:40:--:QA Guest')
elif 'IP4.ADDRESS,IP4.GATEWAY,IP4.DNS' in s:print('192.0.2.10/24\\n192.0.2.1\\n192.0.2.53')
''')
(home/'bin/pactl').write_text('''#!/usr/bin/env python3
import sys,os,json
from pathlib import Path
a=sys.argv[1:];home=Path(os.environ['HOME']);path=home/'audio.json'
try:state=json.loads(path.read_text())
except FileNotFoundError:state={'volume':65,'muted':False,'stream':55,'stream_mute':False,'mic':45,'mic_mute':False}
with open(home/'commands.log','a') as f:f.write('AUDIO '+' '.join(a)+'\\n')
if a[0]=='get-sink-volume':print('Volume: front-left: 42598 / %d%% / -11 dB'%state['volume'])
elif a[0]=='get-sink-mute':print('Mute: '+('yes' if state['muted'] else 'no'))
elif a[0]=='get-default-sink':print('qa-speakers')
elif a==['list','sinks']:print('Sink #1\\nName: qa-speakers\\nDescription: QA Speakers')
elif a==['list','sink-inputs']:print('Sink Input #0\\nMute: %s\\nVolume: front-left: 42598 / %d%% / -11 dB\\napplication.name = "SDE Music QA"\\nmedia.name = "Alpha test track"\\napplication.process.binary = "sdeqa"'%('yes' if state['stream_mute'] else 'no',state['stream']))
elif a==['list','sources']:print('Source #1\\nName: qa-microphone\\nDescription: QA Microphone')
elif a[0]=='get-source-volume':print('Volume: front-left: 42598 / %d%% / -11 dB'%state['mic'])
elif a[0]=='get-source-mute':print('Mute: '+('yes' if state['mic_mute'] else 'no'))
elif a[0]=='get-default-source':print('qa-microphone')
elif a[0]=='set-sink-input-volume':state['stream']=int(a[2].strip('%'))
elif a[0]=='set-sink-input-mute':state['stream_mute']=a[2]=='1'
elif a[0]=='set-sink-volume':state['volume']=int(a[2].strip('%'))
elif a[0]=='set-sink-mute':state['muted']=not state['muted']
elif a[0]=='set-source-volume':state['mic']=int(a[2].strip('%'))
elif a[0]=='set-source-mute':state['mic_mute']=not state['mic_mute']
if a[0].startswith('set-'):path.write_text(json.dumps(state))
''')
for f in (home/'bin').iterdir():f.chmod(0o755)
try:
    if a.tcp_bus:
        conf=home/'bus.conf';conf.write_text('<busconfig><type>session</type><listen>tcp:host=127.0.0.1,port=0,family=ipv4</listen><auth>ANONYMOUS</auth><allow_anonymous/><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
        bus=sp.Popen(['dbus-daemon','--nofork','--print-address','--config-file='+str(conf)],stdout=sp.PIPE,text=True);processes.append(bus);e['DBUS_SESSION_BUS_ADDRESS']=bus.stdout.readline().strip()
    assert e.get('DBUS_SESSION_BUS_ADDRESS'),'Run under dbus-run-session or use --tcp-bus'
    # Deliberately route only this QA process tree's system lookups to its fixture bus.
    e['DBUS_SYSTEM_BUS_ADDRESS']=e['DBUS_SESSION_BUS_ADDRESS']
    transport=['-nolisten','unix','-nolisten','local','-listen','tcp','-ac'] if a.tcp_display else ['-nolisten','tcp','-ac']
    config();xv=spawn('Xvfb',':96','-screen','0',a.size+'x24',*transport);time.sleep(.6);assert xv.poll() is None
    spawn('sde-desktop');panel_proc=spawn('sde-panel');panel=find('^SDE Panel$');time.sleep(.6)
    fixture=spawn(str(a.test_dir.resolve()/'sde-panel-service-fixture'),str(home/'service.log'))
    waitfor(lambda:(home/'service.log').exists() and log_has('READY'),'fixture not ready')
    sni=spawn(str(a.test_dir.resolve()/'sde-sni-demo'))
    icon=find('^SDE XEmbed fixture$',visible=False);host=parent(icon);waitfor(lambda:parent(str(host))==int(panel),'tray not docked in panel')
    time.sleep(1);shot('00-panel-indicators')
    ac=open_section('overview');bounds(ac,panel)
    waitfor(lambda:parent(str(host))==int(ac),'native tray did not move to Action Center')
    click(icon,8/scale,8/scale)
    waitfor(lambda:log_has('XEMBED click'),'native child did not receive pointer input')
    shot('01-action-center')
    # SNI shares the row immediately left of the XEmbed host; activate with a real pointer.
    hg=geom(str(host));run('xdotool','mousemove',hg['x']-round(18*scale),hg['y']+hg['h']//2,'click','1')
    waitfor(lambda:'item: Activate' in (home/'sde-sni-demo.log').read_text(),'SNI Activate missing')
    run('xdotool','key','Escape');time.sleep(.4)
    assert parent(str(host))==int(panel),'tray failed to return on Escape'
    pg=geom(panel);click(panel,pg['w']/scale-80,pg['h']/scale/2)
    ac=find('^Action Center$');time.sleep(.4);bounds(ac,panel)
    run('xdotool','key','Escape');time.sleep(.4)
    ac=open_section('network');bounds(ac,panel);shot('02-network-lan-wifi')
    run('xdotool','mousemove','10','10','click','1');time.sleep(.4)
    assert parent(str(host))==int(panel),'outside click failed to restore tray'
    ac=open_section('bluetooth');bounds(ac,panel);shot('03-bluetooth')
    # Derive row positions from the native tray host, robust to scale/scrollbar rounding.
    hg=geom(str(host));g=geom(ac);base=(hg['y']-g['y'])/scale+(hg['h']/scale)+30
    # Bluetooth title+adapter text+power row precede scan. Capture identifies QA data.
    below_tray(ac,180,90)
    waitfor(lambda:log_has('BT Connect'),'Bluetooth connect command missing')
    below_tray(ac,180,90)
    waitfor(lambda:log_has('BT Disconnect'),'Bluetooth disconnect command missing')
    mode('unpaired');below_tray(ac,180,90)
    waitfor(lambda:log_has('BT Pair'),'Bluetooth pair command missing')
    below_tray(ac,180,90)
    below_tray(ac,72,90)
    waitfor(lambda:log_has('BT Powered off'),'Bluetooth power-off command missing')
    below_tray(ac,72,90)
    waitfor(lambda:log_has('BT Powered on'),'Bluetooth power-on command missing')
    below_tray(ac,100,90)
    waitfor(lambda:log_has('BT StartDiscovery'),'Bluetooth discovery command missing')
    run('xdotool','key','Escape');time.sleep(.5)
    waitfor(lambda:log_has('BT StopDiscovery'),'discovery did not stop on close')
    ac=open_section('media');bounds(ac,panel);shot('04-media-and-volume')
    hg=geom(str(host));g=geom(ac);base=(hg['y']-g['y'])/scale+(hg['h']/scale)+30
    below_tray(ac,100)
    waitfor(lambda:log_has('MPRIS PlayPause'),'MPRIS play/pause command missing')
    # Previous is unsupported by the fixture and must not be sent.
    below_tray(ac,100,60);assert not log_has('MPRIS Previous')
    shot('05-media-paused')
    mode('no-control');below_tray(ac,100)
    assert (home/'service.log').read_text().count('MPRIS PlayPause')==1,'CanControl=false ignored'
    run('xdotool','key','Escape')
    config();
    with (home/'.config/sde/sde.conf').open('a') as f:f.write('show_media=false\n')
    reload();ac=open_section('sound');bounds(ac,panel);shot('06-sound-microphone-apps')
    below_tray(ac,48,210)
    waitfor(lambda:'AUDIO set-sink-volume' in (home/'commands.log').read_text(),'master volume command missing')
    below_tray(ac,188,100)
    waitfor(lambda:'AUDIO set-source-mute' in (home/'commands.log').read_text(),'microphone mute command missing')
    below_tray(ac,230,210)
    waitfor(lambda:'AUDIO set-source-volume' in (home/'commands.log').read_text(),'microphone volume command missing')
    below_tray(ac,381,110)
    waitfor(lambda:'AUDIO set-sink-input-volume 0 ' in (home/'commands.log').read_text(),'per-application volume command missing')
    below_tray(ac,381,330)
    waitfor(lambda:'AUDIO set-sink-input-mute 0 1' in (home/'commands.log').read_text(),'per-application mute command missing')
    shot('06-sound-mixer-scrolled')
    # Refreshes must keep the native host and its client alive in place.
    for _ in range(3):time.sleep(1)
    assert parent(icon)==host and parent(str(host))==int(ac)
    mode('no-adapter');time.sleep(.6);shot('07-no-bluetooth-adapter');assert panel_proc.poll() is None
    run('xdotool','key','Escape');time.sleep(.4)
    for style,top in [('short',False),('floating',True),('full',True)]:
        config(style,top,theme='dark');reload();ac=open_section('overview');bounds(ac,panel,top);shot('08-'+style+('-top' if top else '-bottom'));run('xdotool','key','Escape');time.sleep(.3)
    config(moved=False);reload();ac=open_section('tray');time.sleep(.5);assert parent(str(host))==int(panel),'move_status=false ignored';shot('09-keep-status-on-panel');run('xdotool','key','Escape')
    fixture.terminate();fixture.wait();sni.terminate();sni.wait();time.sleep(.6)
    assert panel_proc.poll() is None,'service disappearance crashed panel'
    ac=open_section('bluetooth');shot('10-services-disconnected');run('xdotool','key','Escape')
    print('PANEL: corner placement, status/tray transfer, native XEmbed/SNI clicks, MPRIS capabilities, Bluetooth discovery/cleanup, absent hardware, refresh and panel styles PASS',flush=True)
finally:
    for proc in reversed(processes):
        if proc.poll() is None:proc.terminate()
    for proc in processes:
        try:proc.wait(timeout=3)
        except sp.TimeoutExpired:proc.kill();proc.wait()
    for f in home.glob('*.log'):
        (out/f.name).write_text(f.read_text())
    tmp.cleanup()
