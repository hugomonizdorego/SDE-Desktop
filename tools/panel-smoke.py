#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native panel integration checks with isolated QA D-Bus/audio/network fixtures.

The panel and its popups describe their named controls in SDE_PANEL_QA_DIR
(see qa_dump in src/panel/popup.h); the checks click those controls with real
pointer events, so they follow the layout at every size and scale.
"""
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
p.add_argument('--display',type=int,default=96)
p.add_argument('--tcp-display',action='store_true')
p.add_argument('--tcp-bus',action='store_true',help='Private localhost bus for containers without Unix sockets')
a=p.parse_args();W,H=map(int,a.size.split('x'));scale=a.scale/100
out=a.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
tmp=tempfile.TemporaryDirectory(prefix='sde-panel-qa-');home=Path(tmp.name);(home/'bin').mkdir();(home/'.config/sde').mkdir(parents=True);(home/'qa').mkdir()
display=f':{a.display}'
e=os.environ.copy();e.update(HOME=str(home),XDG_CONFIG_HOME=str(home/'.config'),XDG_CONFIG_DIRS=str(home/'system'),DISPLAY='127.0.0.1'+display if a.tcp_display else display,LANG='C.UTF-8',LC_ALL='C.UTF-8',SDE_PANEL_QA_DIR=str(home/'qa'),PATH=str(home/'bin')+':'+str(a.bin_dir.resolve())+':'+e['PATH'])
processes=[]
FLYOUTS=['Sound','Network','Bluetooth','Power','Media','Drives','Keyboard','Tray']
def spawn(*cmd):
    log=open(home/(Path(cmd[0]).name+'.log'),'w');proc=sp.Popen(cmd,env=e,stdout=log,stderr=sp.STDOUT);processes.append(proc);return proc
def run(*cmd,check=True):return sp.run(list(map(str,cmd)),env=e,check=check,stdout=sp.PIPE,stderr=sp.PIPE,text=True)
def waitfor(fn,why,tries=100):
    for _ in range(tries):
        value=fn()
        if value:return value
        time.sleep(.08)
    print('Processes', [(proc.args,proc.poll()) for proc in processes],flush=True)
    print(run('xdotool','search','--onlyvisible','--name','.',check=False).stdout,flush=True)
    raise AssertionError(why)
def visible(name):
    r=run('xdotool','search','--onlyvisible','--name',name,check=False)
    return r.stdout.splitlines()[-1] if r.returncode==0 and r.stdout.strip() else None
def find(name,visible_only=True):
    def search():
        args=['xdotool','search']+(['--onlyvisible'] if visible_only else [])+['--name',name];r=run(*args,check=False)
        return r.stdout.splitlines()[-1] if r.returncode==0 and r.stdout.strip() else None
    return waitfor(search,'window not found: '+name)
def gone(name):waitfor(lambda:not visible(name),'window still open: '+name)
def geom(w):
    text=run('xwininfo','-id',w).stdout
    return {k:int(re.search(re.escape(label)+r'\s*:\s*(-?\d+)',text).group(1)) for k,label in [('x','Absolute upper-left X'),('y','Absolute upper-left Y'),('w','Width'),('h','Height')]}
def parent(w):return int(re.search(r'Parent window id:\s*(0x[0-9a-f]+)',run('xwininfo','-tree','-id',w).stdout).group(1),16)
def click(w,x,y,button=1):
    g=geom(w);run('xdotool','mousemove',g['x']+round(x*scale),g['y']+round(y*scale),'click',button);time.sleep(.4)
def shot(name):
    time.sleep(.3);run('import','-window','root',out/(name+'.png'));print('saved',name,flush=True)
def config(style='full',top=False,moved=True,theme='light',look='aero',extra=''):
    (home/'.config/sde/sde.conf').write_text(f'[appearance]\nscale={a.scale}\ntheme={theme}\nstyle={look}\n[panel]\nstyle={style}\nposition={"top" if top else "bottom"}\n[action_center]\nmove_status={str(moved).lower()}\n{extra}')
def conf():return (home/'.config/sde/sde.conf').read_text()
def reload():
    run('xprop','-root','-f','_SDE_CONFIG_SERIAL','32c','-set','_SDE_CONFIG_SERIAL',time.time_ns()%1000000000);time.sleep(.6)
def layout(title):
    """Named controls of a window: name -> (x, y, w, h, active, scrolled)."""
    try:text=(home/'qa'/(title.replace(' ','-')+'.txt')).read_text()
    except FileNotFoundError:return {}
    items={}
    for line in text.splitlines():
        f=line.split('\t')
        if len(f)>=6:items.setdefault(f[0],tuple(int(v) for v in f[1:6])+((int(f[6]) if len(f)>6 else 0),))
    return items
def item(title,name,active=True):
    return waitfor(lambda:(lambda r:r if r and (not active or r[4]) else None)(layout(title).get(name)),f'{title}: no {"active " if active else ""}control {name!r}')
def press(title,name,fx=.5,fy=.5,active=True):
    """Clicks a named control, scrolling the popup body until it is in view."""
    win=find('^'+re.escape(title)+'$')
    for _ in range(40):
        # Service data arriving after the popup opened can rebuild and move
        # it; click only once its geometry and layout are settled.
        for _ in range(20):
            g=geom(win);L=layout(title);time.sleep(.15)
            if geom(win)==g and layout(title)==L:break
        r=item(title,name,active);L=layout(title)
        body=L.get('@body')
        cy=r[1]+r[3]*fy
        if not r[5] or not body or body[1]+2<=cy<=body[1]+body[3]-2:
            run('xdotool','mousemove',g['x']+round(r[0]+r[2]*fx),g['y']+round(r[1]+r[3]*fy),'click','1');time.sleep(.5);return
        run('xdotool','mousemove',g['x']+body[0]+round(4*scale),g['y']+body[1]+body[3]//2,'click','5' if cy>body[1]+body[3] else '4');time.sleep(.2)
    shot('debug-press');raise AssertionError(f'{title}: could not reveal {name!r}')
def in_view(title,name):
    """The control's top and most of it (all of it when it fits) are in view."""
    r=layout(title).get(name);body=layout(title).get('@body')
    if not r or not body or r[1]<body[1]-2:return False
    shown=min(r[1]+r[3],body[1]+body[3])-r[1]
    return shown>=min(r[3]-2,body[3]*.85)
def panel_button(name):
    r=waitfor(lambda:layout('SDE Panel').get(name),'panel button missing: '+name);g=geom(panel)
    return g['x']+r[0]+r[2]//2,g['y']+r[1]+r[3]//2
def click_panel(name,expect=None):
    # A real click on a panel button; one retry absorbs a click that raced
    # the previous popup closing.
    for attempt in range(2):
        x,y=panel_button(name);run('xdotool','mousemove',x,y,'click','1');time.sleep(.3)
        if not expect:return
        for _ in range(40):
            w=visible(expect)
            if w:return w
            time.sleep(.08)
    return find(expect)
def bounds(ac,top=False):
    """The Action Center sits in the panel's corner, on screen."""
    g=geom(ac);pg=geom(panel)
    assert g['x']+g['w']==pg['x']+pg['w'],(g,pg)
    assert g['y']==pg['y']+pg['h'] if top else g['y']+g['h']==pg['y'],(g,pg)
    assert g['x']>=0 and g['y']>=0 and g['x']+g['w']<=W and g['y']+g['h']<=H,(g,pg)
def flyout_bounds(fl,section,top=False):
    """A flyout sits above (below a top panel) its status icon, fully on screen."""
    g=geom(fl);pg=geom(panel);margin=round(8*scale)
    assert g['x']>=0 and g['y']>=0 and g['x']+g['w']<=W and g['y']+g['h']<=H,(section,g)
    if top:assert pg['y']+pg['h']<=g['y']<=pg['y']+pg['h']+2*margin,(section,g,pg)
    else:assert pg['y']-2*margin<=g['y']+g['h']<=pg['y'],(section,g,pg)
    r=layout('SDE Panel').get('status:'+section)
    if r:
        center=pg['x']+r[0]+r[2]/2
        clamped=g['x']<=margin+1 or g['x']+g['w']>=W-margin-1
        assert clamped or abs(g['x']+g['w']/2-center)<=2,(section,g,center)
    assert g['w']<=round(362*scale),(section,g)
def only_open(title):
    for other in FLYOUTS+['Action Center']:
        if other!=title:assert not visible('^'+other+'$'),f'{other} open together with {title}'
def open_section(name,title):
    run('sde-panel','--section',name);w=find('^'+title+'$');time.sleep(.6);return w
def escape(title):
    run('xdotool','key','Escape');gone('^'+title+'$');time.sleep(.2)
def log_has(text):return text in (home/'service.log').read_text()
def commands(text):return text in (home/'commands.log').read_text()
def mode(name):
    run('dbus-send','--session','--type=method_call','--print-reply','--dest=org.bluez','/SDETest','org.sde.Test.Mode','string:'+name);time.sleep(.8)
# Start menu fixture: applications in every main category, more than fit the
# "All Programs" list.
APPS='Atlas Bluefish Calendar Draw Editor Gimp Hex Inkscape Journal Kate Lyrics Mail Notes Orca Paint Quod Rhythm Sudoku Terminal Umbrello Vlc Writer Xterm Yelp Zeal Abacus Bolt Chess Dia Evince Geany Htop Iris Jigsaw Kicad Mines Nano Okular Pidgin Ristretto Shotwell Uget Viewer Wget Xournal'.split()
CATS=['Utility','Graphics','Network','AudioVideo','Office','Development','Education','Game','Science','Settings','System']
(home/'.local/share/applications').mkdir(parents=True)
for i,n in enumerate(APPS):
    (home/f'.local/share/applications/qa-{n.lower()}.desktop').write_text(f'[Desktop Entry]\nType=Application\nName={n}\nGenericName={n} QA\nExec=true\nIcon=applications-other\nCategories={CATS[i%len(CATS)]};\n')
def menu_scroll():return layout('SDE Menu')['@all-scroll'][0]
def menu_checks():
    """Start menu: chips fit, slim size, wheel/scrollbar/keyboard scrolling,
    letter jump, frecency "Recently Used", 12 favorites, search."""
    m=click_panel('start','^SDE Menu$');time.sleep(1.2);g=geom(m)
    assert g['w']<=round(580*scale)+1 and g['x']>=0 and g['y']>=0 and g['x']+g['w']<=W and g['y']+g['h']<=H,g
    chips=item('SDE Menu','@chips');L=layout('SDE Menu')
    cats=[v for k,v in L.items() if k.startswith('category:')]
    assert len(cats)>=10,L.keys()
    for v in cats:assert chips[0]<=v[0] and v[0]+v[2]<=chips[0]+chips[2]<=g['w'] and chips[1]<=v[1] and v[1]+v[3]<=chips[1]+chips[3],(v,chips)
    assert 'files' in L and not any(k in L for k in ['Settings','Lock','Power']),L.keys()
    shot('20-menu')
    # Mouse wheel over "All Programs".
    a=item('SDE Menu','@all');s=menu_scroll()
    run('xdotool','mousemove',g['x']+a[0]+a[2]//2,g['y']+a[1]+a[3]//2)
    for _ in range(3):run('xdotool','click','5');time.sleep(.1)
    waitfor(lambda:menu_scroll()>s,'wheel did not scroll All Programs');s=menu_scroll()
    run('xdotool','click','4');waitfor(lambda:menu_scroll()<s,'wheel up did not scroll back')
    # Scrollbar drag.
    bx=g['x']+a[0]+a[2]-round(4*scale);s=menu_scroll()
    run('xdotool','mousemove',bx,g['y']+a[1]+round(12*scale),'mousedown','1');time.sleep(.2)
    run('xdotool','mousemove',bx,g['y']+a[1]+a[3]//2);time.sleep(.2);run('xdotool','mouseup','1');time.sleep(.3)
    waitfor(lambda:menu_scroll()>s+a[3]//2,'scrollbar drag did not scroll');s=menu_scroll()
    shot('21-menu-scrolled')
    # Page Down / Page Up.
    run('xdotool','key','Next');waitfor(lambda:menu_scroll()>s,'Page Down did not scroll');s=menu_scroll()
    run('xdotool','key','Prior');waitfor(lambda:menu_scroll()<s,'Page Up did not scroll')
    # Letter heading -> jump grid -> letter.
    press('SDE Menu',next(k for k in layout('SDE Menu') if k.startswith('letter:')))
    press('SDE Menu','jump:W');waitfor(lambda:'app:Wget' in layout('SDE Menu'),'letter jump did not reach W')
    shot('22-menu-letter-jump')
    # Launching counts for "Recently Used".
    press('SDE Menu','app:Wget');gone('^SDE Menu$')
    assert 'qa-wget.desktop\t1\t' in (home/'.local/state/sde/menu-usage').read_text()
    m=click_panel('start','^SDE Menu$');time.sleep(.8)
    waitfor(lambda:'recent:Wget' in layout('SDE Menu'),'launched app missing from Recently Used')
    # Search still finds settings pages and session actions.
    run('xdotool','type','shut down');waitfor(lambda:'result:Log Out, Restart or Shut Down' in layout('SDE Menu'),'session action not found')
    shot('23-menu-search')
    # Escape clears the search, then leaves its page, then closes the menu.
    for _ in range(2):run('xdotool','key','Escape');time.sleep(.3)
    escape('SDE Menu')
    # Twelve favorites fill the single page; a 13th replaces the oldest.
    favs=';'.join(f'qa-{n.lower()}.desktop' for n in APPS[:12])
    config(extra=f'[menu]\nfavorites={favs}\n');reload()
    m=click_panel('start','^SDE Menu$');time.sleep(.8)
    waitfor(lambda:sum(k.startswith('favorite:') for k in layout('SDE Menu'))==12,'12 favorites not shown')
    shot('24-menu-12-favorites')
    press('SDE Menu',next(k for k in layout('SDE Menu') if k.startswith('letter:')))
    press('SDE Menu','jump:Z');r=item('SDE Menu','app:Zeal');g=geom(m)
    run('xdotool','mousemove',g['x']+r[0]+r[2]//2,g['y']+r[1]+r[3]//2,'click','3');time.sleep(.6)
    shot('25-menu-favorites-full')
    run('xdotool','key','Down','Down','Return');time.sleep(.6)
    waitfor(lambda:'qa-zeal.desktop' in conf(),'13th favorite not added')
    text=conf();assert 'qa-atlas.desktop' not in text and text.count('.desktop')==12,text
    escape('SDE Menu')
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
        conf_file=home/'bus.conf';conf_file.write_text('<busconfig><type>session</type><listen>tcp:host=127.0.0.1,port=0,family=ipv4</listen><auth>ANONYMOUS</auth><allow_anonymous/><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
        bus=sp.Popen(['dbus-daemon','--nofork','--print-address','--config-file='+str(conf_file)],stdout=sp.PIPE,text=True);processes.append(bus);e['DBUS_SESSION_BUS_ADDRESS']=bus.stdout.readline().strip()
    assert e.get('DBUS_SESSION_BUS_ADDRESS'),'Run under dbus-run-session or use --tcp-bus'
    # Deliberately route only this QA process tree's system lookups to its fixture bus.
    e['DBUS_SYSTEM_BUS_ADDRESS']=e['DBUS_SESSION_BUS_ADDRESS']
    transport=['-nolisten','unix','-nolisten','local','-listen','tcp','-ac'] if a.tcp_display else ['-nolisten','tcp','-ac']
    config();xv=spawn('Xvfb',display,'-screen','0',a.size+'x24',*transport);time.sleep(.6);assert xv.poll() is None
    spawn('sde-desktop');panel_proc=spawn('sde-panel');panel=find('^SDE Panel$');time.sleep(.6)
    fixture=spawn(str(a.test_dir.resolve()/'sde-panel-service-fixture'),str(home/'service.log'))
    waitfor(lambda:(home/'service.log').exists() and log_has('READY'),'fixture not ready')
    sni=spawn(str(a.test_dir.resolve()/'sde-sni-demo'))
    icon=find('^SDE XEmbed fixture$',visible_only=False);host=parent(icon);waitfor(lambda:parent(str(host))==int(panel),'tray not docked in panel')
    waitfor(lambda:'status:bluetooth' in layout('SDE Panel') and 'status:media' in layout('SDE Panel'),'status icons missing')
    time.sleep(1);shot('00-panel-indicators')
    menu_checks();config();reload()

    # The overflow icon opens the Tray flyout, which gathers the native tray.
    fl=click_panel('status:tray','^Tray$');time.sleep(.6);flyout_bounds(fl,'tray');only_open('Tray')
    waitfor(lambda:parent(str(host))==int(fl),'native tray did not move to the Tray flyout')
    click(icon,8/scale,8/scale)
    waitfor(lambda:log_has('XEMBED click'),'native child did not receive pointer input')
    shot('01-tray-flyout')
    # SNI shares the row immediately left of the XEmbed host; activate with a real pointer.
    hg=geom(str(host));run('xdotool','mousemove',hg['x']-round(18*scale),hg['y']+hg['h']//2,'click','1')
    waitfor(lambda:'item: Activate' in (home/'sde-sni-demo.log').read_text(),'SNI Activate missing')
    escape('Tray')
    assert parent(str(host))==int(panel),'tray failed to return on Escape'

    # The corner button opens the Action Center (expanded by default).
    ac=click_panel('action-center','^Action Center$');time.sleep(.5);bounds(ac);only_open('Action Center')
    for name in ['Wi-Fi','Bluetooth','Airplane mode','Do Not Disturb','Dark Mode','Display','Settings','Output volume','Calendar','Show less','Lock Screen']:
        item('Action Center',name,active=False)
    tall=geom(ac)['h'];shot('02-action-center')
    # Compact mode: tiles and sliders only, remembered in sde.conf.
    press('Action Center','Show less')
    waitfor(lambda:'expanded=false' in conf(),'compact mode not saved')
    waitfor(lambda:'Calendar' not in layout('Action Center') and 'Show more' in layout('Action Center'),'compact mode still shows the calendar')
    time.sleep(.4);ac=find('^Action Center$');assert geom(ac)['h']<tall or tall>=H-round(80*scale),(geom(ac),tall);bounds(ac)
    shot('03-action-center-compact')
    run('xdotool','mousemove','10','10','click','1');gone('^Action Center$')
    # The clock opens the Action Center with the calendar in view, even in compact mode.
    ac=click_panel('clock','^Action Center$');time.sleep(.5);bounds(ac)
    waitfor(lambda:in_view('Action Center','Calendar'),'calendar not in view after clicking the clock')
    shot('04-calendar-from-clock')
    # One click on the corner button switches to the remembered (compact) view.
    click_panel('action-center');time.sleep(.6);ac=find('^Action Center$')
    waitfor(lambda:'Calendar' not in layout('Action Center'),'corner button did not switch to the compact view')
    press('Action Center','Show more')
    waitfor(lambda:'expanded=true' in conf(),'expanded mode not saved')
    item('Action Center','Calendar',active=False)
    # The corner button closes what it opened.
    click_panel('action-center');gone('^Action Center$')
    run('sde-panel','--calendar');ac=find('^Action Center$');time.sleep(.5)
    waitfor(lambda:in_view('Action Center','Calendar'),'sde-panel --calendar did not show the calendar')
    escape('Action Center')

    # Each status icon opens its own flyout above it; another icon switches
    # with one click, the same icon closes it.
    for section,title in [('sound','Sound'),('network','Network'),('bluetooth','Bluetooth'),('media','Media'),('keyboard','Keyboard')]:
        fl=click_panel('status:'+section,'^'+title+'$');time.sleep(.5)
        flyout_bounds(fl,section);only_open(title);shot('05-flyout-'+section)
    click_panel('status:keyboard');gone('^Keyboard$')
    ac=click_panel('action-center','^Action Center$');time.sleep(.3)
    fl=click_panel('status:sound','^Sound$');gone('^Action Center$');only_open('Sound')
    # The Action Center's sound chevron opens the Sound flyout.
    escape('Sound');ac=click_panel('action-center','^Action Center$')
    press('Action Center','Sound output and applications');find('^Sound$');gone('^Action Center$');escape('Sound')

    fl=open_section('network','Network');flyout_bounds(fl,'network')
    for name in ['Turn Wi-Fi Off','Airplane mode','Scan Wi-Fi networks','QA Library · Secured','Disconnect LAN']:item('Network',name,active=False)
    shot('06-network-lan-wifi')
    run('xdotool','mousemove','10','10','click','1');gone('^Network$')

    fl=open_section('bluetooth','Bluetooth');flyout_bounds(fl,'bluetooth');shot('07-bluetooth')
    press('Bluetooth','Connect');waitfor(lambda:log_has('BT Connect'),'Bluetooth connect command missing')
    press('Bluetooth','Disconnect');waitfor(lambda:log_has('BT Disconnect'),'Bluetooth disconnect command missing')
    mode('unpaired');press('Bluetooth','Pair and connect')
    waitfor(lambda:log_has('BT Pair'),'Bluetooth pair command missing')
    press('Bluetooth','Turn Bluetooth Off');waitfor(lambda:log_has('BT Powered off'),'Bluetooth power-off command missing')
    press('Bluetooth','Turn Bluetooth On');waitfor(lambda:log_has('BT Powered on'),'Bluetooth power-on command missing')
    press('Bluetooth','Scan for devices');waitfor(lambda:log_has('BT StartDiscovery'),'Bluetooth discovery command missing')
    escape('Bluetooth')
    waitfor(lambda:log_has('BT StopDiscovery'),'discovery did not stop on close')

    fl=open_section('media','Media');flyout_bounds(fl,'media');shot('08-media')
    press('Media','Pause');waitfor(lambda:log_has('MPRIS PlayPause'),'MPRIS play/pause command missing')
    # Previous is unsupported by the fixture and must not be sent.
    press('Media','Previous',active=False);time.sleep(.4);assert not log_has('MPRIS Previous')
    shot('08-media-paused')
    mode('no-control');item('Media','Play',active=False);press('Media','Play',active=False);time.sleep(.4)
    assert (home/'service.log').read_text().count('MPRIS PlayPause')==1,'CanControl=false ignored'
    escape('Media')

    config(extra='show_media=false\n');reload()
    fl=open_section('sound','Sound');flyout_bounds(fl,'sound');shot('09-sound-microphone-apps')
    press('Sound','Output volume',.3);waitfor(lambda:commands('AUDIO set-sink-volume'),'master volume command missing')
    press('Sound','Mute microphone');waitfor(lambda:commands('AUDIO set-source-mute'),'microphone mute command missing')
    press('Sound','Microphone volume',.3);waitfor(lambda:commands('AUDIO set-source-volume'),'microphone volume command missing')
    press('Sound','SDE Music QA',.3);waitfor(lambda:commands('AUDIO set-sink-input-volume 0 '),'per-application volume command missing')
    press('Sound','Mute application');waitfor(lambda:commands('AUDIO set-sink-input-mute 0 1'),'per-application mute command missing')
    shot('09-sound-mixer-scrolled')
    escape('Sound')
    for section,title in [('power','Power'),('drives','Drives'),('keyboard','Keyboard')]:
        fl=open_section(section,title);flyout_bounds(fl,section);shot('10-flyout-'+section);escape(title)

    # Refreshes must keep the native host and its client alive in place.
    fl=open_section('tray','Tray');waitfor(lambda:parent(str(host))==int(fl),'tray did not move')
    for _ in range(3):time.sleep(1)
    assert parent(icon)==host and parent(str(host))==int(fl)
    escape('Tray')
    mode('no-adapter');fl=open_section('bluetooth','Bluetooth');time.sleep(.6);shot('11-no-bluetooth-adapter');assert panel_proc.poll() is None
    escape('Bluetooth')

    for style,top in [('short',False),('floating',True),('full',True)]:
        config(style,top,theme='dark');reload()
        run('sde-panel','--action-center');ac=find('^Action Center$');time.sleep(.5);bounds(ac,top);shot('12-'+style+('-top' if top else '-bottom'));escape('Action Center')
        fl=open_section('network','Network');flyout_bounds(fl,'network',top);shot('12-'+style+('-top' if top else '-bottom')+'-flyout');escape('Network')
    for theme,look in [('light','sde'),('dark','sde'),('light','aero'),('dark','aero'),('light','liquid-glass'),('dark','liquid-glass')]:
        config(theme=theme,look=look);reload()
        run('sde-panel','--action-center');ac=find('^Action Center$');time.sleep(.5);bounds(ac);shot(f'13-theme-{look}-{theme}');escape('Action Center')
        fl=open_section('sound','Sound');flyout_bounds(fl,'sound');shot(f'13-theme-{look}-{theme}-sound');escape('Sound')
    config(moved=False);reload();fl=open_section('tray','Tray');time.sleep(.5);assert parent(str(host))==int(panel),'move_status=false ignored';shot('14-keep-tray-on-panel');escape('Tray')
    fixture.terminate();fixture.wait();sni.terminate();sni.wait();time.sleep(.6)
    assert panel_proc.poll() is None,'service disappearance crashed panel'
    fl=open_section('bluetooth','Bluetooth');shot('15-services-disconnected');escape('Bluetooth')
    run('sde-panel','--action-center');find('^Action Center$');escape('Action Center')
    assert panel_proc.poll() is None,'panel exited'
    print('PANEL: Action Center (compact/expanded, calendar, corner), status flyouts anchored above their icons, one-click switching, tray transfer, native XEmbed/SNI clicks, MPRIS capabilities, Bluetooth discovery/cleanup, absent hardware, refresh, panel styles and themes PASS',flush=True)
finally:
    for proc in reversed(processes):
        if proc.poll() is None:proc.terminate()
    for proc in processes:
        try:proc.wait(timeout=3)
        except sp.TimeoutExpired:proc.kill();proc.wait()
    for f in home.glob('*.log'):
        (out/f.name).write_text(f.read_text())
    tmp.cleanup()
