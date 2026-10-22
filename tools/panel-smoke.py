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
e=os.environ.copy();e.update(HOME=str(home),XDG_CONFIG_HOME=str(home/'.config'),XDG_CONFIG_DIRS=str(home/'system'),XDG_CACHE_HOME=str(home/'.cache'),DISPLAY='127.0.0.1'+display if a.tcp_display else display,LANG='C.UTF-8',LC_ALL='C.UTF-8',SDE_PANEL_QA_DIR=str(home/'qa'),SDE_BACKLIGHT_DIR=str(home/'backlight'),PATH=str(home/'bin')+':'+str(a.bin_dir.resolve())+':'+e['PATH'])
# The Action Center's weather comes from a fixed report (no network in QA).
e['SDE_WEATHER_FIXTURE']=os.environ.get('SDE_WEATHER_FIXTURE','rain')
processes=[]
# A backlight the panel may write to (see SDE_BACKLIGHT_DIR in quick.cpp).
(home/'backlight/qa_backlight').mkdir(parents=True);(home/'backlight/qa_backlight/brightness').write_text('60\n');(home/'backlight/qa_backlight/max_brightness').write_text('100\n')
FLYOUTS=['Sound','Network','Bluetooth','Power','Media','Drives','Keyboard','Tray','Alarms']
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
def config(style='full',top=False,moved=True,theme='light',look='graphite',extra='',overflow=True):
    # overflow: one tray icon is already "many", so the fixture's two icons
    # gather behind the overflow button (the default threshold is 4).
    limit='tray_overflow_threshold=1\n' if overflow else ''
    (home/'.config/sde/sde.conf').write_text(f'[appearance]\nscale={a.scale}\ntheme={theme}\nstyle={look}\n[panel]\nstyle={style}\nposition={"top" if top else "bottom"}\n{limit}[action_center]\nmove_status={str(moved).lower()}\n{extra}')
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
        run('xdotool','mousemove',g['x']+body[0]+round(4*scale),g['y']+body[1]+body[3]//2,'click','5' if cy>body[1]+body[3]-2 else '4');time.sleep(.2)
    print('press failed',name,layout(title).get(name),layout(title).get('@body'),flush=True)
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
def command(args,title):
    # A command sent with sde-panel can be lost when it races the previous
    # popup closing (seen with alpha14 too, ~2 in 80); send it once more.
    for attempt in range(3):
        if visible('^'+title+'$'):return visible('^'+title+'$')
        run('sde-panel',*args)
        for _ in range(40):
            w=visible('^'+title+'$')
            if w:return w
            time.sleep(.08)
    return find('^'+title+'$')
def open_section(name,title):
    w=command(['--section',name],title);time.sleep(.6);return w
def escape(title):
    run('xdotool','key','Escape');gone('^'+title+'$');time.sleep(.2)
def log_has(text):return text in (home/'service.log').read_text()
def commands(text):
    try:return text in (home/'commands.log').read_text()
    except FileNotFoundError:return False
def mapped(w):return 'IsViewable' in run('xwininfo','-id',str(w)).stdout
def type_into(title,name,text):
    press(title,name);run('xdotool','type','--delay','30',text);time.sleep(.4)
def notified(text):
    try:return text in (home/'dbus-monitor.log').read_text()
    except FileNotFoundError:return False
def mode(name):
    run('dbus-send','--session','--type=method_call','--print-reply','--dest=org.bluez','/SDETest','org.sde.Test.Mode','string:'+name);time.sleep(.8)
# Start menu fixture: applications in every main category, more than fit the
# "All Programs" list.
APPS='Atlas Bluefish Calendar Draw Editor Gimp Hex Inkscape Journal Kate Lyrics Mail Notes Orca Paint Quod Rhythm Sudoku Terminal Umbrello Vlc Writer Xterm Yelp Zeal Abacus Bolt Chess Dia Evince Geany Htop Iris Jigsaw Kicad Mines Nano Okular Pidgin Ristretto Shotwell Uget Viewer Wget Xournal'.split()
CATS=['Utility','Graphics','Network','AudioVideo','Office','Development','Education','Game','Science','Settings','System']
(home/'.local/share/applications').mkdir(parents=True)
for i,n in enumerate(APPS):
    (home/f'.local/share/applications/qa-{n.lower()}.desktop').write_text(f'[Desktop Entry]\nType=Application\nName={n}\nGenericName={n} QA\nExec=true\nIcon=applications-other\nCategories={CATS[i%len(CATS)]};\n')
def menu_scroll():return layout('SDE Menu')['@list-scroll'][0]
def inside(r,area):return area[0]<=r[0] and r[0]+r[2]<=area[0]+area[2]
def drag(win,src,dst):
    g=geom(win);run('xdotool','mousemove',g['x']+src[0],g['y']+src[1],'mousedown','1');time.sleep(.2)
    for k in range(1,6):
        run('xdotool','mousemove',g['x']+src[0]+(dst[0]-src[0])*k//5,g['y']+src[1]+(dst[1]-src[1])*k//5);time.sleep(.08)
    time.sleep(.2);run('xdotool','mouseup','1');time.sleep(.6)
def center(r):return (r[0]+r[2]//2,r[1]+r[3]//2)
def rail_checks():
    """Start menu rail (alpha50): Gallery opens sde-gallery; the account
    shows only the account page (no Lock or Log Out); Power off asks first
    and is the menu's only session action."""
    press('SDE Menu','rail:gallery');gone('^SDE Menu$')
    waitfor(lambda:commands('GALLERY'),'Gallery did not start sde-gallery')
    m=click_panel('start','^SDE Menu$');time.sleep(.8)
    press('SDE Menu','rail:account');time.sleep(.6);shot('20-menu-account')
    run('xdotool','key','Escape');time.sleep(.4)
    if visible('^SDE Menu$'):escape('SDE Menu')
    # Power off: Escape cancels the question, Enter powers off.
    m=click_panel('start','^SDE Menu$');time.sleep(.8)
    press('SDE Menu','rail:power');gone('^SDE Menu$');d=find('^Power off$');time.sleep(.4)
    shot('20-menu-power-off-question');run('xdotool','key','Escape');gone('^Power off$')
    time.sleep(.3);assert not commands('LOGOUT --action poweroff'),'Power off ran without asking'
    m=click_panel('start','^SDE Menu$');time.sleep(.8)
    press('SDE Menu','rail:power');find('^Power off$');time.sleep(.4)
    run('xdotool','key','Return');gone('^Power off$')
    waitfor(lambda:commands('LOGOUT --action poweroff'),'Power off did not run sde-logout --action poweroff')
    assert not commands('LOGOUT --action logout') and not commands('LOGOUT --action lock'),'menu ran another session action'
def menu_checks():
    """Start menu: rail, A-Z list with a category filter, live-tile groups
    (drag, resize, move, rename), resizing the menu, typing goes to search."""
    m=click_panel('start','^SDE Menu$');time.sleep(1.2);g=geom(m)
    assert g['x']>=0 and g['y']>=0 and g['x']+g['w']<=W and g['y']+g['h']<=H,g
    L=layout('SDE Menu')
    for name in ['rail:menu','rail:account','rail:documents','rail:pictures','rail:gallery','rail:settings','rail:power','@list','@tiles']:assert name in L,(name,L.keys())
    assert any(k.startswith('filter:') for k in L) and any(k.startswith('group:') for k in L),L.keys()
    # No search field and no row of category chips any more.
    assert 'search' not in L and not any(k.startswith('category:') for k in L),L.keys()
    lst,tiles=L['@list'],L['@tiles']
    assert L['rail:menu'][0]+L['rail:menu'][2]<=lst[0]<lst[0]+lst[2]<=tiles[0],(lst,tiles)
    for k,v in L.items():
        if k.startswith('tile:'):assert inside(v,tiles),(k,v,tiles)
        if k.startswith('app:'):assert inside(v,lst),(k,v,lst)
    # The rail sits at the bottom: account, Documents, Pictures, Gallery,
    # Settings, then (apart) the one Power off button.
    order=['account','documents','pictures','gallery','settings','power']
    ys=[L['rail:'+n][1] for n in order];assert ys==sorted(ys) and ys[-1]+L['rail:power'][3]<=g['h'],ys
    assert L['rail:power'][1]-(L['rail:settings'][1]+L['rail:settings'][3])>=round(6*scale),'Power off not set apart'
    shot('20-menu')
    # The menu button expands the rail with labels; Escape folds it.
    press('SDE Menu','rail:menu');waitfor(lambda:'@rail-open' in layout('SDE Menu'),'rail did not expand')
    shot('20-menu-rail');run('xdotool','key','Escape');waitfor(lambda:'@rail-open' not in layout('SDE Menu'),'rail did not fold')
    rail_checks()
    m=click_panel('start','^SDE Menu$');time.sleep(1.0);g=geom(m)
    # Mouse wheel over the list.
    a=item('SDE Menu','@list');s=menu_scroll()
    run('xdotool','mousemove',g['x']+a[0]+a[2]//2,g['y']+a[1]+a[3]//2)
    for _ in range(3):run('xdotool','click','5');time.sleep(.1)
    waitfor(lambda:menu_scroll()>s,'wheel did not scroll the list');s=menu_scroll()
    run('xdotool','click','4');waitfor(lambda:menu_scroll()<s,'wheel up did not scroll back')
    # Scrollbar drag.
    bx=g['x']+a[0]+a[2]-round(4*scale);s=menu_scroll()
    run('xdotool','mousemove',bx,g['y']+a[1]+round(12*scale),'mousedown','1');time.sleep(.2)
    run('xdotool','mousemove',bx,g['y']+a[1]+a[3]//2);time.sleep(.2);run('xdotool','mouseup','1');time.sleep(.3)
    waitfor(lambda:menu_scroll()>s+a[3]//2,'scrollbar drag did not scroll');s=menu_scroll()
    shot('21-menu-scrolled')
    # Page Down / Page Up move through the list.
    run('xdotool','mousemove',g['x']+a[0]+a[2]//2,g['y']+a[1]+a[3]//2);time.sleep(.2)
    run('xdotool','key','Next');waitfor(lambda:menu_scroll()>s,'Page Down did not scroll');s=menu_scroll()
    run('xdotool','key','Prior');waitfor(lambda:menu_scroll()<s,'Page Up did not scroll')
    # Letter heading -> jump grid -> letter.
    press('SDE Menu',next(k for k in layout('SDE Menu') if k.startswith('letter:')))
    press('SDE Menu','jump:W');waitfor(lambda:'app:Wget' in layout('SDE Menu'),'letter jump did not reach W')
    shot('22-menu-letter-jump')
    # The category filter: a short menu; the list keeps only that category.
    press('SDE Menu',next(k for k in layout('SDE Menu') if k.startswith('filter:')));time.sleep(.4)
    shot('22-menu-filter');run('xdotool','key','Down','Down','Return')
    waitfor(lambda:'filter:Accessories' in layout('SDE Menu'),'category filter not applied')
    L=layout('SDE Menu');assert 'app:Atlas' in L and 'app:Bluefish' not in L and not any(k.startswith('added:') for k in L),L.keys()
    shot('22-menu-filtered');run('xdotool','key','Escape');waitfor(lambda:'filter:All apps' in layout('SDE Menu'),'Escape did not clear the filter')
    # Launching counts for "Most used".
    press('SDE Menu',next(k for k in layout('SDE Menu') if k.startswith('letter:')));press('SDE Menu','jump:W')
    press('SDE Menu','app:Wget');gone('^SDE Menu$')
    assert 'qa-wget.desktop\t1\t' in (home/'.local/state/sde/menu-usage').read_text()
    m=click_panel('start','^SDE Menu$');time.sleep(.8)
    waitfor(lambda:'most-used:Wget' in layout('SDE Menu'),'launched app missing from Most used')
    # Typing in the menu moves to the panel's search.
    run('xdotool','type','--delay','40','shut down');gone('^SDE Menu$')
    waitfor(lambda:'result:Log Out, Restart or Shut Down' in layout('SDE Search'),'typing in the menu did not search')
    assert 'query:shut down' in layout('SDE Search'),layout('SDE Search').keys()
    shot('23-menu-typing-searches');escape('SDE Search')
    # alpha15 favorites become one tile group.
    favs=';'.join(f'qa-{n.lower()}.desktop' for n in APPS[:12])
    config(extra=f'[menu]\nfavorites={favs}\n');reload()
    m=click_panel('start','^SDE Menu$');time.sleep(.8)
    waitfor(lambda:sum(k.startswith('tile:') for k in layout('SDE Menu'))>=6,'migrated favorites missing')
    L=layout('SDE Menu');assert [k for k in L if k.startswith('group:')]==['group:Productivity'],L.keys()
    assert 'tiles=' not in conf();escape('SDE Menu')
    # Tile groups and sizes.
    config(extra='[menu]\ntiles=Work,qa-atlas.desktop:m,qa-bluefish.desktop:m,qa-calendar.desktop:w;Play,qa-chess.desktop:s,qa-sudoku.desktop:l\n');reload()
    m=click_panel('start','^SDE Menu$');time.sleep(.8);g=geom(m)
    L=item('SDE Menu','tile:Calendar') and layout('SDE Menu')
    assert 'group:Work' in L,L.keys()
    med,wide=L['tile:Atlas'],L['tile:Calendar']
    shot('24-menu-tiles')
    # The second group may be below the fold on small screens: scroll the
    # tiles with the wheel to reach it.
    def reveal(name):
        for _ in range(16):
            L=layout('SDE Menu')
            if name in L:return L
            t=L['@tiles'];g=geom(m)
            run('xdotool','mousemove',g['x']+t[0]+t[2]//2,g['y']+t[1]+t[3]//2,'click','5');time.sleep(.3)
        raise AssertionError('tile not reachable: '+name)
    L=reveal('tile:Sudoku');small,large=L['tile:Chess'],L['tile:Sudoku']
    assert wide[2]>med[2]*1.8 and abs(wide[3]-med[3])<=1 and small[2]<med[2]*.6 and large[3]>med[3]*1.8,(med,wide,small,large)
    assert 'group:Play' in L and L['group:Play'][1]<small[1],L.keys()
    shot('24-menu-tiles-play')
    for _ in range(6):run('xdotool','click','4');time.sleep(.1)
    waitfor(lambda:layout('SDE Menu')['@tiles-scroll'][0]==0,'tiles did not scroll back')
    # Drag the wide tile in front of Bluefish.
    L=layout('SDE Menu');blue=L['tile:Bluefish'];wide=L['tile:Calendar']
    drag(m,center(wide),(blue[0]+2,blue[1]+blue[3]//2))
    waitfor(lambda:'Work,qa-atlas.desktop:m,qa-calendar.desktop:w,qa-bluefish.desktop:m' in conf(),'tile drag not saved: '+conf())
    # Right-click a tile: Resize > Large.
    r=item('SDE Menu','tile:Atlas');g=geom(m)
    run('xdotool','mousemove',g['x']+r[0]+r[2]//2,g['y']+r[1]+r[3]//2,'click','3');time.sleep(.6)
    shot('25-menu-tile-menu')
    run('xdotool','key','Down','Down','Down','Right','Down','Down','Down','Return');time.sleep(.6)
    waitfor(lambda:'qa-atlas.desktop:l' in conf(),'tile resize not saved: '+conf())
    # Rename a group by clicking its name.
    press('SDE Menu','group:Work');time.sleep(.3)
    run('xdotool','key','ctrl+a');run('xdotool','type','--delay','30','Games');run('xdotool','key','Return')
    waitfor(lambda:'tiles=Games,' in conf(),'group rename not saved: '+conf())
    # Drag an application from the list to the tiles: a new group.
    L=layout('SDE Menu');row=next(v for k,v in L.items() if k.startswith('app:'));name=next(k for k,v in L.items() if k.startswith('app:'))[4:]
    t=L['@tiles'];drag(m,center(row),(t[0]+t[2]//2,t[1]+t[3]-round(20*scale)))
    waitfor(lambda:f'qa-{name.lower()}.desktop:m' in conf(),'app dragged to the tiles not pinned: '+conf())
    shot('26-menu-tiles-arranged')
    # Drag the right edge to make the menu narrower; the size is kept.
    g=geom(m);w0=g['w']
    run('xdotool','mousemove',g['x']+g['w']-2,g['y']+g['h']//2,'mousedown','1');time.sleep(.2)
    run('xdotool','mousemove',g['x']+g['w']-round(100*scale),g['y']+g['h']//2);time.sleep(.3);run('xdotool','mouseup','1');time.sleep(.5)
    waitfor(lambda:geom(m)['w']<w0-20,'menu did not resize');waitfor(lambda:'width=' in conf(),'menu size not saved')
    shot('27-menu-resized');escape('SDE Menu')
    m=click_panel('start','^SDE Menu$');time.sleep(.6);assert geom(m)['w']<w0-20,'menu size not kept';escape('SDE Menu')
    config();reload()
def search_checks():
    """Panel search: box with placeholder, flyout with results, keyboard,
    icon and hidden styles."""
    # A full panel shows the box; on a narrow screen it becomes an icon and
    # the flyout shows the field itself.
    box=waitfor(lambda:'search' if 'search' in layout('SDE Panel') else 'search-icon' if 'search-icon' in layout('SDE Panel') else None,'no search on the panel')
    s=click_panel(box,'^SDE Search$');time.sleep(.6)
    L=layout('SDE Search')
    assert 'query:' in L,L.keys()
    assert ('field' in L)==(box=='search-icon') and any(k.startswith('top:') or k.startswith('chip:') for k in L),L.keys()
    pg=geom(panel);sg=geom(s);assert sg['y']+sg['h']<=pg['y'] and sg['x']>=0 and sg['x']+sg['w']<=W,(sg,pg)
    shot('28-search-start')
    run('xdotool','type','--delay','40','wget');waitfor(lambda:'result:Wget' in layout('SDE Search'),'app not found')
    assert any(k.startswith('action:') for k in layout('SDE Search'))
    shot('29-search-results')
    run('xdotool','key','Return');gone('^SDE Search$')
    waitfor(lambda:'qa-wget.desktop\t2\t' in (home/'.local/state/sde/menu-usage').read_text(),'Enter did not open the best match')
    # Files, keyboard selection and the session actions.
    (home/'Documents').mkdir(exist_ok=True);(home/'Documents/QA Report.txt').write_text('x')
    s=command(['--search','report'],'SDE Search');waitfor(lambda:'result:QA Report.txt' in layout('SDE Search'),'file not found')
    press('SDE Search','tab:Files');waitfor(lambda:all(not k.startswith('result:') or k=='result:QA Report.txt' for k in layout('SDE Search')),'Files tab shows other results')
    escape('SDE Search')
    s=command(['--search','shut down'],'SDE Search');waitfor(lambda:'selected:Log Out, Restart or Shut Down' in layout('SDE Search'),'session action not selected')
    run('xdotool','key','Return');gone('^SDE Search$');waitfor(lambda:commands('LOGOUT'),'session action did not run')
    # Short and floating panels show a search icon; the flyout has the field.
    config(style='floating');reload()
    waitfor(lambda:'search-icon' in layout('SDE Panel') and 'search' not in layout('SDE Panel'),'floating panel without search icon')
    s=click_panel('search-icon','^SDE Search$');time.sleep(.5);item('SDE Search','field')
    run('xdotool','type','--delay','40','zeal');waitfor(lambda:'result:Zeal' in layout('SDE Search'),'search icon flyout did not search')
    shot('30-search-icon');escape('SDE Search')
    config(extra='[panel]\nsearch=hidden\n');reload()
    waitfor(lambda:not any(k.startswith('search') for k in layout('SDE Panel')),'hidden search still on the panel')
    config();reload();item('SDE Panel',box)
def weather_checks():
    """Action Center (alpha50): the weather sky behind the clock with the
    temperature, condition and place; [weather] enabled=false removes it."""
    L=waitfor(lambda:(lambda L:L if any(k.startswith('@weather-') for k in L) else None)(layout('Action Center')),'weather picture missing')
    kind=next(k for k in L if k.startswith('@weather-'))
    assert kind=='@weather-'+{'rain':'rain-day','clear-night':'clear-night','snow':'snow-day','thunder':'thunder-day'}.get(e['SDE_WEATHER_FIXTURE'],kind[9:]),kind
    tip=next(k for k in L if k.startswith('Weather: '));assert 'Jakarta' in tip and '°C' in tip,tip
    w=L[kind];assert w[1]>=0 and w[1]+w[3]<=L['@body'][1],('weather card leaves the header',w,L['@body'])
    shot('02-action-center-weather')
    escape('Action Center')
    config(extra='[weather]\nenabled=false\n');reload()
    command(['--action-center'],'Action Center');time.sleep(.5)
    waitfor(lambda:not any(k.startswith('@weather-') for k in layout('Action Center')),'weather shown while disabled')
    escape('Action Center');config(overflow=False);reload()
    command(['--action-center'],'Action Center');time.sleep(.5)
def brightness_settings_checks():
    """The brightness row's gear: Night Light (gamma), schedule, auto-dim on
    battery and dim level for sde-power, the DDC/CI monitor, Display settings."""
    press('Action Center','Brightness settings')
    for name in ['Night Light','Night Light warmth','Night Light schedule','Auto-dim on battery','Dim level','External monitor brightness','Display settings…','Close brightness settings']:
        item('Action Center',name,active=False)
    shot('02-action-center-brightness-settings')
    press('Action Center','Night Light');waitfor(lambda:'[night_light]' in conf() and 'enabled=true' in conf().split('[night_light]')[1],'Night Light not saved')
    press('Action Center','Night Light schedule');time.sleep(.4);run('xdotool','key','Down','Return');time.sleep(.5)
    waitfor(lambda:'schedule=always' in conf(),'Night Light schedule not saved')
    waitfor(lambda:(home/'qa/night-light.txt').exists() and (home/'qa/night-light.txt').read_text().startswith('on 4000'),'Night Light not applied')
    shot('02-action-center-night-light')
    press('Action Center','Night Light warmth',.85)
    waitfor(lambda:re.search(r'temperature=(\d+)',conf()) and int(re.search(r'temperature=(\d+)',conf()).group(1))<4000,'warmth not saved')
    waitfor(lambda:(home/'qa/night-light.txt').read_text().split()[1]!='4000','warmth not applied')
    press('Action Center','Night Light');waitfor(lambda:'enabled=false' in conf().split('[night_light]')[1],'Night Light not switched off')
    waitfor(lambda:(home/'qa/night-light.txt').read_text().startswith('off'),'Night Light not restored')
    press('Action Center','Auto-dim on battery');waitfor(lambda:'idle_dim_battery=0' in conf(),'auto-dim not switched off for sde-power')
    press('Action Center','Auto-dim on battery');waitfor(lambda:'idle_dim_battery=120' in conf(),'auto-dim not restored')
    press('Action Center','Display settings…');gone('^Action Center$')
    waitfor(lambda:commands('SETTINGS --page display'),'Display settings… did not open sde-settings --page display')
    command(['--action-center'],'Action Center');time.sleep(.4)
    assert 'Night Light' not in layout('Action Center'),'brightness settings stayed open'
def media_card_checks():
    """The media card: title, artist (album), cover, seek bar and only
    previous / play-pause / next for the active MPRIS player."""
    item('Action Center','Now playing: Alpha test track',active=False)
    item('Action Center','Artist: SDE QA — QA Sessions',active=False)
    for name in ['Playback position','Pause','Next']:item('Action Center',name)
    item('Action Center','Previous',active=False)
    assert 'Open player' not in layout('Action Center'),'media card shows more than the transport buttons'
    shot('02-action-center-media')
    press('Action Center','Playback position',.5)
    waitfor(lambda:log_has('MPRIS SetPosition /org/sde/qa/track/1 9'),'seek bar did not send SetPosition')
    escape('Action Center')
def brightness_checks(ac):
    """Action Center: the brightness slider follows the volume slider in the
    same card; wheel and brightness commands change the backlight."""
    L=layout('Action Center');v,b=L['Output volume'],L['Brightness']
    assert b[1]>v[1] and b[1]-v[1]<=round(60*scale) and abs(b[0]-v[0])<=2,(v,b)
    def level():
        try:return int((home/'backlight/qa_backlight/brightness').read_text())
        except ValueError:return -1  # being written
    g=geom(ac);before=level()
    # Small screens: scroll the Action Center until the row is in view
    # (scrolling over the row itself would change the brightness).
    for _ in range(12):
        b=layout('Action Center')['Brightness'];body=layout('Action Center')['@body'];cy=b[1]+b[3]//2
        if body[1]<=cy<=body[1]+body[3]-2:break
        run('xdotool','mousemove',g['x']+body[0]+round(4*scale),g['y']+body[1]+round(10*scale),'click','5' if cy>body[1]+body[3] else '4');time.sleep(.3)
    before=level()
    run('xdotool','mousemove',g['x']+b[0]-round(20*scale),g['y']+b[1]+b[3]//2,'click','4');time.sleep(.2);run('xdotool','click','4')
    waitfor(lambda:level()==before+10,f'wheel did not raise the brightness ({level()})')
    # Brightness keys (sde-panel --brightness); a command can race the
    # previous one, so it is sent again while nothing changed.
    for _ in range(3):
        run('sde-panel','--brightness','-20')
        for _ in range(20):
            if level()!=before+10:break
            time.sleep(.1)
        if level()!=before+10:break
    waitfor(lambda:level()==before-10,f'sde-panel --brightness did not change it ({level()})')
    shot('02-action-center-brightness')
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
elif a[:2]==['-t','networking']:print('full')
''')
# Session actions, the browser and sound players only log what they were asked.
for name,tag in [('sde-logout','LOGOUT'),('xdg-open','OPEN'),('canberra-gtk-play','SOUND'),('paplay','SOUND'),('pw-play','SOUND'),('sde-settings','SETTINGS'),('sde-gallery','GALLERY')]:
    (home/'bin'/name).write_text(f'''#!/bin/sh
echo "{tag} $*" >> "$HOME/commands.log"
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
    config(overflow=False);xv=spawn('Xvfb',display,'-screen','0',a.size+'x24',*transport);time.sleep(.6);assert xv.poll() is None
    spawn('sde-notifyd');time.sleep(.3)
    spawn('dbus-monitor','--session',"interface='org.freedesktop.Notifications'")
    spawn('sde-desktop');find('^SDE Desktop$');time.sleep(.3);panel_proc=spawn('sde-panel');panel=find('^SDE Panel$');time.sleep(.6)
    fixture=spawn(str(a.test_dir.resolve()/'sde-panel-service-fixture'),str(home/'service.log'))
    waitfor(lambda:(home/'service.log').exists() and log_has('READY'),'fixture not ready')
    sni=spawn(str(a.test_dir.resolve()/'sde-sni-demo'))
    icon=find('^SDE XEmbed fixture$',visible_only=False);host=parent(icon);waitfor(lambda:parent(str(host))==int(panel),'tray not docked in panel')
    waitfor(lambda:'status:bluetooth' in layout('SDE Panel') and 'status:media' in layout('SDE Panel'),'status icons missing')
    time.sleep(1);shot('00-panel-indicators')
    menu_checks()
    search_checks()
    # Few tray icons: they sit on the panel and there is no overflow button.
    config(overflow=False);reload()
    waitfor(lambda:'status:tray' not in layout('SDE Panel'),'overflow button shown for two tray icons')
    waitfor(lambda:parent(str(host))==int(panel) and mapped(host),'tray icons not shown on the panel')
    shot('00-panel-tray-direct')
    # Many icons (threshold 1 here): they gather behind the overflow button.
    config();reload()
    waitfor(lambda:'status:tray' in layout('SDE Panel'),'overflow button missing with many tray icons')
    waitfor(lambda:not mapped(host),'tray icons still on the panel behind the overflow button')
    shot('00-panel-tray-overflow')

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
    for name in ['Wi-Fi','Bluetooth','Airplane mode','Do Not Disturb','Dark Mode','Display','Alarms','Output volume','Brightness','Brightness settings','Calendar','Add agenda item','Show less','SDE Settings','Lock Screen','Power','Action Center settings']:
        item('Action Center',name,active=False)
    tall=geom(ac)['h'];shot('02-action-center')
    brightness_checks(ac)
    weather_checks()
    brightness_settings_checks()
    media_card_checks()
    ac=command(['--action-center'],'Action Center');time.sleep(.4)
    # Compact mode: tiles and sliders only, remembered in sde.conf.
    press('Action Center','Show less')
    waitfor(lambda:'expanded=false' in conf(),'compact mode not saved')
    waitfor(lambda:'Calendar' not in layout('Action Center') and 'Show more' in layout('Action Center'),'compact mode still shows the calendar')
    time.sleep(.4);ac=find('^Action Center$');assert geom(ac)['h']<tall or tall>=H-round(80*scale),(geom(ac),tall);bounds(ac)
    shot('03-action-center-compact')
    run('xdotool','mousemove','10','10','click','1');gone('^Action Center$')
    # The clock opens its alarms popup; its calendar link opens the Action
    # Center with the calendar in view, even in compact mode.
    al=click_panel('clock','^Alarms$');time.sleep(.5);only_open('Alarms');flyout_bounds(al,'clock')
    shot('04-alarms')
    press('Alarms','Calendar and agenda');ac=find('^Action Center$');gone('^Alarms$');bounds(ac)
    waitfor(lambda:in_view('Action Center','Calendar'),'calendar not in view after the alarms link')
    shot('04-calendar-from-clock')
    # One click on the corner button switches to the remembered (compact) view.
    click_panel('action-center');time.sleep(.6);ac=find('^Action Center$')
    waitfor(lambda:'Calendar' not in layout('Action Center'),'corner button did not switch to the compact view')
    press('Action Center','Show more')
    waitfor(lambda:'expanded=true' in conf(),'expanded mode not saved')
    item('Action Center','Calendar',active=False)
    # The corner button closes what it opened.
    click_panel('action-center');gone('^Action Center$')
    ac=command(['--calendar'],'Action Center');time.sleep(.5)
    waitfor(lambda:in_view('Action Center','Calendar'),'sde-panel --calendar did not show the calendar')
    escape('Action Center')

    # Alarms: add one (time, repeat day, name), switch it off, ring, snooze.
    al=click_panel('clock','^Alarms$');time.sleep(.4)
    press('Alarms','Add alarm');item('Alarms','Hour up')
    press('Alarms','Hour up');press('Alarms','Minute up');press('Alarms','Monday');press('Alarms','Saturday')
    type_into('Alarms','Alarm name','QA Wake');shot('16-alarm-editor')
    press('Alarms','Save')
    waitfor(lambda:'label=QA Wake' in (home/'.config/sde/alarms.ini').read_text(),'alarm not saved')
    text=(home/'.config/sde/alarms.ini').read_text();assert 'days=mon,sat' in text and 'enabled=true' in text,text
    when=re.search(r'time=(\d\d:\d\d)',text).group(1)
    item('Alarms','Edit alarm '+when);shot('17-alarm-list')
    press('Alarms','Alarm '+when+' on')
    waitfor(lambda:'enabled=false' in (home/'.config/sde/alarms.ini').read_text(),'alarm switch not saved')
    escape('Alarms')
    # A snooze that ends in two seconds rings: notification, sound, Snooze/Dismiss.
    due=int(time.time())+2
    (home/'.config/sde/alarms.ini').write_text(text+f'\n[alarm 9]\ntime=06:00\nlabel=QA Ring\nsnooze=5\nsnoozed_until={due}\n');reload()
    waitfor(lambda:notified('QA Ring'),'alarm notification missing',tries=150)
    waitfor(lambda:commands('SOUND'),'alarm sound missing')
    assert notified('snooze') and notified('dismiss'),'alarm notification without Snooze/Dismiss'
    time.sleep(.6);shot('18-alarm-ringing')
    al=click_panel('clock','^Alarms$');press('Alarms','Snooze')
    waitfor(lambda:re.search(r'snoozed_until=(\d+)',(home/'.config/sde/alarms.ini').read_text()) and int(re.search(r'snoozed_until=(\d+)',(home/'.config/sde/alarms.ini').read_text()).group(1))>=due+290,'snooze not saved')
    waitfor(lambda:notified('CloseNotification'),'ringing notification not closed on snooze')
    item('Alarms','Add alarm');assert 'Dismiss' not in layout('Alarms');escape('Alarms')

    # Agenda: pick today, add an item with a reminder; the calendar marks the day.
    ac=command(['--calendar'],'Action Center');time.sleep(.5)
    press('Action Center','Add agenda item')
    type_into('Action Center','Agenda title','QA Dentist')
    type_into('Action Center','Agenda time','23:58')
    type_into('Action Center','Agenda note','Bring the card')
    press('Action Center','Agenda reminder');time.sleep(.4);run('xdotool','key','Down','Down','Down','Return');time.sleep(.5)
    shot('19-agenda-form')
    press('Action Center','Save item')
    agenda=home/'.local/share/sde/agenda.ini'
    waitfor(lambda:agenda.exists() and 'title=QA Dentist' in agenda.read_text(),'agenda item not saved')
    t=agenda.read_text();assert 'time=23:58' in t and 'note=Bring the card' in t and 'remind=15' in t and time.strftime('date=%Y-%m-%d') in t,t
    item('Action Center','Edit agenda item QA Dentist',active=False);shot('20-agenda-item')
    escape('Action Center')
    # A reminder that is due fires as a notification, also with the Action Center closed.
    soon=time.localtime(time.time()+120)
    agenda.write_text(t+f'\n[item 50]\ndate={time.strftime("%Y-%m-%d",soon)}\ntime={time.strftime("%H:%M",soon)}\ntitle=QA Standup\nremind=5\n');reload()
    waitfor(lambda:notified('QA Standup'),'agenda reminder notification missing',tries=150)
    waitfor(lambda:'reminded=true' in agenda.read_text(),'reminder not marked as fired')

    # Session buttons live in the Action Center footer.
    ac=command(['--action-center'],'Action Center');time.sleep(.4)
    press('Action Center','Power')
    for name in ['Sleep','Log Out','Restart','Shut Down','Close power options']:item('Action Center',name)
    shot('21-power-options')
    press('Action Center','Restart');gone('^Action Center$')
    waitfor(lambda:commands('LOGOUT --action reboot'),'Restart did not run sde-logout --action reboot')
    ac=command(['--action-center'],'Action Center');time.sleep(.4)
    assert 'Restart' not in layout('Action Center'),'power options stayed open'
    press('Action Center','Lock Screen');gone('^Action Center$')
    waitfor(lambda:commands('LOGOUT --action lock'),'Lock Screen did not run sde-logout --action lock')
    ac=command(['--action-center'],'Action Center');time.sleep(.4)
    press('Action Center','SDE Settings');gone('^Action Center$')
    waitfor(lambda:commands('SETTINGS \n'),'SDE Settings button did not start sde-settings')

    # Internet connectivity from NetworkManager: limited, captive portal, full.
    mode('nm-limited');fl=open_section('network','Network')
    item('Network','Connected, no internet',active=False)
    waitfor(lambda:log_has('NM CheckConnectivity'),'opening the Network flyout did not check connectivity')
    shot('22-network-no-internet')
    press('Network','Troubleshoot')
    waitfor(lambda:commands('NM connection up id QA Wired LAN'),'Troubleshoot did not reconnect')
    escape('Network');shot('22-panel-no-internet')
    mode('nm-portal');fl=open_section('network','Network')
    item('Network','Sign in to network',active=False);shot('23-network-portal')
    press('Network','Open sign-in page');gone('^Network$')
    waitfor(lambda:commands('OPEN http://nmcheck.example.test/'),'portal sign-in page not opened')
    mode('nm-full');fl=open_section('network','Network')
    item('Network','Connected · Internet access',active=False);escape('Network')

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
        ac=command(['--action-center'],'Action Center');time.sleep(.5);bounds(ac,top);shot('12-'+style+('-top' if top else '-bottom'));escape('Action Center')
        fl=open_section('network','Network');flyout_bounds(fl,'network',top);shot('12-'+style+('-top' if top else '-bottom')+'-flyout');escape('Network')
    # SDE theme families (alpha16) with two accents; "look" may carry the accent line.
    for theme,look in [('light','graphite'),('dark','graphite\naccent_name=green'),('light','fluent'),('dark','fluent\naccent_name=purple'),('light','glass'),('dark','glass')]:
        config(theme=theme,look=look);reload()
        ac=command(['--action-center'],'Action Center');time.sleep(.5);bounds(ac);shot(f'13-theme-{look.split(chr(10))[0]}-{theme}');escape('Action Center')
        fl=open_section('sound','Sound');flyout_bounds(fl,'sound');shot(f'13-theme-{look.split(chr(10))[0]}-{theme}-sound');escape('Sound')
    config(moved=False);reload();fl=open_section('tray','Tray');time.sleep(.5);assert parent(str(host))==int(panel),'move_status=false ignored';shot('14-keep-tray-on-panel');escape('Tray')
    fixture.terminate();fixture.wait();sni.terminate();sni.wait();time.sleep(.6)
    assert panel_proc.poll() is None,'service disappearance crashed panel'
    fl=open_section('bluetooth','Bluetooth');shot('15-services-disconnected');escape('Bluetooth')
    command(['--action-center'],'Action Center');escape('Action Center')
    assert panel_proc.poll() is None,'panel exited'
    print('PANEL: tray overflow only for many icons, clock alarms (edit, ring, notify, sound, snooze), agenda with reminders, power footer, NM connectivity (limited/portal/full, troubleshoot), Action Center (compact/expanded, calendar, corner), status flyouts anchored above their icons, one-click switching, tray transfer, native XEmbed/SNI clicks, MPRIS capabilities, Bluetooth discovery/cleanup, absent hardware, refresh, panel styles and themes PASS',flush=True)
finally:
    for proc in reversed(processes):
        if proc.poll() is None:proc.terminate()
    for proc in processes:
        try:proc.wait(timeout=3)
        except sp.TimeoutExpired:proc.kill();proc.wait()
    for f in home.glob('*.log'):
        (out/f.name).write_text(f.read_text())
    tmp.cleanup()
