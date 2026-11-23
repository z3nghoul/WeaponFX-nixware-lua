-- @name        WeaponFX
-- @author      z3n1337
-- @version     2.0
-- @description WeaponFX Loader: custom viewmodel chams

-- WeaponFX (MIT, Garagoro/WeaponFX) draws custom HLSL shaders on the local
-- viewmodel only. Place WeaponFX.dll and the shaders/ folder in:
--   C:\nix\scripts\WeaponFX\
-- Custom .hlsl shaders dropped into C:\nix\scripts\WeaponFX\shaders\ are
-- picked up automatically on script load.
--
-- On start the script checks GitHub for a newer WeaponFX.dll (SHA-256 of the
-- local file vs. the latest release) and shows a notice if they differ.

local ffi = require('ffi')

ffi.cdef[[
typedef void* HMODULE;
typedef struct { uint32_t lo, hi; } WFX_FILETIME;
typedef struct {
    uint32_t     dwFileAttributes;
    WFX_FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    uint32_t     nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char         cFileName[260];
    char         cAlternateFileName[14];
} WFX_FIND_DATAA;

HMODULE  GetModuleHandleA(const char* name);
HMODULE  LoadLibraryA(const char* path);
void*    GetProcAddress(HMODULE module, const char* name);
uint32_t GetLastError(void);
void*    FindFirstFileA(const char* pattern, WFX_FIND_DATAA* data);
int      FindNextFileA(void* find, WFX_FIND_DATAA* data);
int      FindClose(void* find);
uint32_t GetFileAttributesA(const char* path);
]]

ffi.cdef[[
typedef void* HINTERNET;
HINTERNET InternetOpenA(const char* agent, uint32_t access, const char* proxy, const char* bypass, uint32_t flags);
HINTERNET InternetOpenUrlA(HINTERNET session, const char* url, const char* headers, uint32_t headers_len, uint32_t flags, uintptr_t context);
int       InternetReadFile(HINTERNET file, void* buffer, uint32_t size, uint32_t* read);
int       InternetSetOptionA(HINTERNET handle, uint32_t option, void* buffer, uint32_t size);
int       InternetCloseHandle(HINTERNET handle);
int32_t   BCryptHash(void* alg, const uint8_t* secret, uint32_t secret_len, const uint8_t* input, uint32_t input_len, uint8_t* output, uint32_t output_len);
void*     ShellExecuteA(void* hwnd, const char* verb, const char* file, const char* params, const char* dir, int show);
]]

local C = ffi.C
local DLL_NAME = 'WeaponFX.dll'

local function file_exists(path)
    return path ~= nil and path ~= '' and C.GetFileAttributesA(path) ~= 0xFFFFFFFF
end

-- Nixware keeps scripts in %SystemDrive%\nix\scripts (documented default: C:)
local DATA_DIR = (os.getenv('SystemDrive') or 'C:') .. '\\nix\\scripts\\WeaponFX'
local SHADER_DIR = DATA_DIR .. '\\shaders'
local INI_PATH = DATA_DIR .. '\\weaponfx.ini'
local DLL_PATH = DATA_DIR .. '\\' .. DLL_NAME
local COMPONENTS = { x = 1, y = 2, z = 3, w = 4 }

local function log(text) print('[weaponfx] ' .. text) end

-- Shaders -------------------------------------------------------------------

local function list_shader_files()
    local out = {}
    local data = ffi.new('WFX_FIND_DATAA')
    local h = C.FindFirstFileA(SHADER_DIR .. '\\*.hlsl', data)
    if ffi.cast('intptr_t', h) == -1 then return out end
    repeat
        local name = ffi.string(data.cFileName)
        if not name:find('^_') and name ~= 'debug.hlsl' and name ~= 'sky.hlsl' then
            out[#out + 1] = name
        end
    until C.FindNextFileA(h, data) == 0
    C.FindClose(h)
    table.sort(out)
    return out
end

local function read_file(path)
    local f = io.open(path, 'rb')
    if not f then return nil end
    local s = f:read('*a')
    f:close()
    return s
end

local function parse_header(source)
    local info = { tex0 = false, params = {} }
    local last, last_indent
    for line in source:gmatch('[^\r\n]*') do
        local body = line:match('^//(.*)$')
        if not body then
            if line:find('%S') then break end
        else
            local indent, rest = body:match('^(%s*)(.*)$')
            local slot, comps, text = rest:match('^param(%d)%.([xyzw]+)%s+(.*)$')
            if slot then
                last = { slot = tonumber(slot), comps = comps, text = text }
                last_indent = #indent
                info.params[#info.params + 1] = last
            elseif rest:find('^tex0%s') then
                info.tex0 = true
                last = nil
            elseif last and #indent > last_indent + 1 and rest ~= '' then
                last.text = last.text .. ' ' .. rest
            else
                last = nil
            end
        end
    end
    return info
end

local function defaults_of(text)
    local out = {}
    local list = text:match('default%s+([%-%d%.%s]+)')
    if list then
        for n in list:gmatch('%-?%d*%.?%d+') do out[#out + 1] = tonumber(n) end
    end
    return out
end

local function title_case(s)
    return s:gsub('(%a)([%w_]*)', function(a, b) return a:upper() .. b end)
end

local function short_label(text)
    local s = text:gsub('%s*%(.*$', ''):gsub('%s*:.*$', '')
    s = title_case(s)
    if #s > 36 then s = s:sub(1, 33) .. '...' end
    return s
end

local function is_colour(p)
    return #p.comps == 3 and (p.text:lower():find('colou?r') or p.text:lower():find('tint')) ~= nil
end

local shader_files = list_shader_files()
local shaders = {}
for _, file in ipairs(shader_files) do
    local src = read_file(SHADER_DIR .. '\\' .. file)
    shaders[#shaders + 1] = {
        file = file,
        key  = file:gsub('%.hlsl$', ''):gsub('[^%w_]', '_'),
        info = src and parse_header(src) or { tex0 = false, params = {} },
    }
end
if #shaders == 0 then
    log('no shaders in ' .. SHADER_DIR .. '; copy the shaders folder there and reload')
    shaders[1] = { file = 'liquid_glass.hlsl', key = 'liquid_glass', info = { tex0 = false, params = {} } }
end

local shader_names = {}
for i, s in ipairs(shaders) do
    shader_names[i] = title_case(s.file:gsub('%.hlsl$', ''):gsub('_', ' '))
end

-- Update check -------------------------------------------------------------
-- Compares SHA-256 of the local WeaponFX.dll with the hash CI writes into the
-- latest release notes. Any failure is silent. Nothing is downloaded.

local RELEASES_API = 'https://api.github.com/repos/z3nghoul/WeaponFX-nixware-lua/releases/latest'
local RELEASES_PAGE = 'https://github.com/z3nghoul/WeaponFX-nixware-lua/releases/latest'
local FORUM_URL = 'https://nixware.cc/workshop/1376/rate' -- review page; the button is hidden while empty
local dll_state = 'checking' -- checking | missing | unknown | outdated | current
local ready = false -- set once the update check is done; settings stay hidden until then

local function fetch_release_body()
    local wininet = ffi.load('wininet')
    local session = wininet.InternetOpenA('WeaponFX-loader', 0, nil, nil, 0)
    if session == nil then return nil end

    local timeout = ffi.new('uint32_t[1]', 3000)
    for _, option in ipairs({ 2, 5, 6 }) do -- connect, send, receive
        wininet.InternetSetOptionA(session, option, timeout, 4)
    end

    local headers = 'Accept: application/vnd.github+json\r\n'
    -- RELOAD | NO_CACHE_WRITE | NO_UI
    local file = wininet.InternetOpenUrlA(session, RELEASES_API, headers, #headers, 0x84000200, 0)
    if file == nil then
        wininet.InternetCloseHandle(session)
        return nil
    end

    local parts, total = {}, 0
    local buf = ffi.new('uint8_t[8192]')
    local got = ffi.new('uint32_t[1]')
    while total < 262144 do
        if wininet.InternetReadFile(file, buf, 8192, got) == 0 or got[0] == 0 then break end
        parts[#parts + 1] = ffi.string(buf, got[0])
        total = total + got[0]
    end
    wininet.InternetCloseHandle(file)
    wininet.InternetCloseHandle(session)
    return table.concat(parts)
end

local function sha256_hex(data)
    local bcrypt = ffi.load('bcrypt')
    local out = ffi.new('uint8_t[32]')
    -- 0x41 = BCRYPT_SHA256_ALG_HANDLE (pseudo handle, Windows 10+)
    if bcrypt.BCryptHash(ffi.cast('void*', 0x41), nil, 0, data, #data, out, 32) ~= 0 then return nil end
    local hex = {}
    for i = 0, 31 do hex[#hex + 1] = ('%02X'):format(out[i]) end
    return table.concat(hex)
end

local function check_for_update()
    dll_state = 'unknown'
    local dll = read_file(DLL_PATH)
    if not dll then
        dll_state = 'missing'
        return
    end
    local local_hash = sha256_hex(dll)
    local body = fetch_release_body()
    if not local_hash or not body then return end
    local remote_hash = body:match('WeaponFX%.dll[^%w]*SHA%-256[^%w]*(%x+)')
    if not remote_hash or #remote_hash ~= 64 then return end
    dll_state = remote_hash:upper() == local_hash and 'current' or 'outdated'
end

local function open_url(url)
    ffi.load('shell32').ShellExecuteA(nil, 'open', url, nil, nil, 1)
end

-- UI: one tab, a tab selector (Home / Weapon / Gloves / Hands / Sleeves) -----

local dirty = true
local function mark_dirty() dirty = true end

local tab = assert(ui.tab('weaponfx', 'WeaponFX'))
local left = tab:section('main_sec', 'General', 'left')
local right = tab:section('side_sec', 'Settings', 'right')

local TARGET_SPECS = {
    { prefix = 'weapon',  label = 'Weapon',  section = 'default', default_on = true  },
    { prefix = 'gloves',  label = 'Gloves',  section = 'gloves',  default_on = false },
    { prefix = 'hands',   label = 'Hands',   section = 'hands',   default_on = false },
    { prefix = 'sleeves', label = 'Sleeves', section = 'sleeves', default_on = false },
}

local tab_names = { 'Home' }
for i, spec in ipairs(TARGET_SPECS) do tab_names[i + 1] = spec.label end
local selector = left:list('tab_selector', 'Tab selector', tab_names, 0)
selector:order(-1)
selector:set(0) -- always start on Home, whatever the config or a reload restored

-- Home: feedback + update status
do
    local function on_home() return selector:get() == 0 end

    local feedback = left:header('feedback_card', 'Feedback')
    feedback:visible_if(on_home)
    feedback:label('Thanks for using WeaponFX!')
    feedback:label('Please leave a review, it helps a lot.')
    feedback:button('forum_open', 'Leave a review', function() open_url(FORUM_URL) end)
        :visible_if(function() return FORUM_URL ~= '' end)

    -- Card captions are fixed, so the outdated state gets a card of its own.
    local function is_loading() return not ready end
    local function is_missing() return ready and dll_state == 'missing' end
    local function is_unknown() return ready and dll_state == 'unknown' end
    local function is_outdated() return ready and dll_state == 'outdated' end
    local function is_current() return ready and dll_state == 'current' end

    local checking = right:header('update_card', 'DLL update')
    checking:visible_if(function() return on_home() and (is_loading() or is_unknown()) end)
    checking:label('Checking for updates...'):visible_if(is_loading)
    checking:label('Update check unavailable.'):visible_if(is_unknown)

    local missing = right:header('update_missing_card', 'DLL NOT FOUND!')
    missing:visible_if(function() return on_home() and is_missing() end)
    missing:label('WeaponFX.dll is missing.')
    missing:label('Download the full package from releases.')
    missing:label('Extract it into the scripts folder.')
    missing:button('missing_open', 'Open releases page', function() open_url(RELEASES_PAGE) end)

    local good = right:header('update_ok_card', 'You are good to go')
    good:visible_if(function() return on_home() and is_current() end)
    good:label('DLL is up to date.')

    local alert = right:header('update_alert_card', 'UPDATE THE DLL!')
    alert:visible_if(function() return on_home() and is_outdated() end)
    alert:label('New release: better stability and bugfixes.')
    alert:button('update_open', 'Open releases page', function() open_url(RELEASES_PAGE) end)
end

local function shader_controls(container, prefix, shader, is_visible)
    local controls = {}
    for _, p in ipairs(shader.info.params) do
        local base = ('%s_%s_p%d%s'):format(prefix, shader.key, p.slot, p.comps)
        local defaults = defaults_of(p.text)
        local label = short_label(p.text)
        if is_colour(p) then
            local on = container:checkbox(base .. '_on', 'Custom ' .. label, false)
            local col = container:color_edit(base .. '_col', label, color(255, 255, 255))
            on:visible_if(is_visible)
            col:visible_if(function() return is_visible() and on:get() end)
            on:on_change(mark_dirty)
            col:on_change(mark_dirty)
            controls[#controls + 1] = { kind = 'colour', slot = p.slot, on = on, col = col }
        else
            local text = p.text:lower()
            for i = 1, #p.comps do
                local comp = p.comps:sub(i, i)
                local d = defaults[i] or defaults[1] or 0
                local max = text:find('0%.%.1') and 1 or (d > 0 and d * 4 or 1)
                local min = (text:find('negative') or text:find('<%s*0')) and -max or 0
                local name = #p.comps > 1 and (label .. ' (' .. comp:upper() .. ')') or label
                local s = container:slider_float(base .. comp, name, d, min, max)
                s:format(max <= 2 and '%.2f' or '%.1f')
                s:tooltip(('param%d.%s - %s'):format(p.slot, comp, p.text))
                s:visible_if(is_visible)
                s:on_change(mark_dirty)
                controls[#controls + 1] = { kind = 'value', slot = p.slot, comp = COMPONENTS[comp], ctl = s }
            end
        end
    end
    return controls
end

local targets = {}
for index, spec in ipairs(TARGET_SPECS) do
    local prefix = spec.prefix
    local sec = right:header(prefix .. '_card', spec.label)
    sec:visible_if(function() return ready and selector:get() == index end)

    local en = sec:checkbox(prefix .. '_en', 'Enable ' .. spec.label, spec.default_on)
    en:on_change(mark_dirty)

    local function is_enabled() return en:get() end

    local sh = sec:combo(prefix .. '_shader', 'Shader', shader_names, 0)
    sh:visible_if(is_enabled)
    sh:on_change(mark_dirty)

    local sep1 = sec:separator()
    sep1:visible_if(is_enabled)

    local per_shader = {}
    for i, s in ipairs(shaders) do
        local function is_shader_visible()
            return en:get() and sh:get() == i - 1
        end
        per_shader[i] = shader_controls(sec, prefix, s, is_shader_visible)
    end

    local tex0 = sec:input_string(prefix .. '_tex0', 'Mask Texture (e.g. masks\\awp_stock.png)', '')
    tex0:text_size(0)
    tex0:visible_if(function()
        if not en:get() then return false end
        local s = shaders[sh:get() + 1]
        return s ~= nil and s.info.tex0
    end)
    tex0:on_change(mark_dirty)

    targets[#targets + 1] = {
        section = spec.section,
        ctl = {
            enable = en,
            shader = sh,
            per_shader = per_shader,
            tex0 = tex0,
        },
    }
end

-- Config file ---------------------------------------------------------------

local function num(v) return (('%.4f'):format(v):gsub('%.?0+$', '')) end

local function channel(c, name, index)
    local v = c[name] or c[index] or 255
    return v / 255
end

local function build_section(t)
    local c = t.ctl
    local index = c.shader:get() + 1
    local shader = shaders[index] or shaders[1]
    local lines = {
        ('[%s]'):format(t.section),
        'mode   = ' .. (c.enable:get() and 'replace' or 'off'),
        'shader = shaders\\' .. shader.file,
    }
    if t.section == 'default' then lines[#lines + 1] = 'vmdepth = 1' end
    local params = { [0] = { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } }
    for _, p in ipairs(c.per_shader[index] or {}) do
        local v = params[p.slot]
        if v and p.kind == 'value' then
            v[p.comp] = p.ctl:get()
        elseif v and p.kind == 'colour' and p.on:get() then
            local col = p.col:get()
            v[1], v[2], v[3] = channel(col, 'r', 1), channel(col, 'g', 2), channel(col, 'b', 3)
        end
    end
    for slot = 0, 3 do
        local v = params[slot]
        lines[#lines + 1] = ('param%d = %s %s %s %s'):format(slot, num(v[1]), num(v[2]), num(v[3]), num(v[4]))
    end
    local tex = c.tex0:get():gsub('[\r\n]', '')
    if shader.info.tex0 and tex ~= '' then lines[#lines + 1] = 'tex0   = ' .. tex end
    return table.concat(lines, '\n')
end

local last_written

local function write_ini()
    local parts = { '; written by WeaponFX - edit from the Nixware menu' }
    for _, t in ipairs(targets) do parts[#parts + 1] = build_section(t) end
    local text = table.concat(parts, '\n\n') .. '\n'
    if text == last_written then return end
    local f, err = io.open(INI_PATH, 'w')
    if not f then
        log('cannot write ' .. INI_PATH .. ': ' .. tostring(err))
        return
    end
    f:write(text)
    f:close()
    last_written = text
end

-- DLL -----------------------------------------------------------------------

local load_failed = false
local next_try = 0
local frames = 0

local function module_handle()
    local h = C.GetModuleHandleA(DLL_NAME)
    if h == nil then return nil end
    return h
end

local function load_dll()
    write_ini()
    local h = C.LoadLibraryA(DLL_PATH)
    if h == nil then
        local err = C.GetLastError()
        load_failed = true
        local hint = err == 126 and (file_exists(DLL_PATH) and ' (missing dependency)' or ' (file not found)') or ''
        log(('LoadLibrary failed for "%s", error %d%s'):format(DLL_PATH, err, hint))
        return
    end
end

local function stop_dll()
    local h = module_handle()
    if not h then return false end
    local stop = C.GetProcAddress(h, 'WfxStop')
    if stop == nil then return false end
    ffi.cast('void(*)(void)', stop)()
    return true
end

-- Callbacks -----------------------------------------------------------------

events.render(function()
    local ok, err = pcall(function()
        if dirty then
            dirty = false
            write_ini()
        end
        if not ready then
            -- Let a few frames draw the "Checking for updates..." text first: the check
            -- blocks the render thread for up to a few seconds on a bad network.
            frames = frames + 1
            if frames < 3 then return end
            pcall(check_for_update)
            ready = true
        end
        local now = os.clock()
        if now < next_try then return end
        next_try = now + 1
        if not module_handle() and not load_failed then
            load_dll()
        end
    end)
    if not ok then log(tostring(err)) end
end)

events.shutdown(function()
    pcall(stop_dll)
end)
