-- @name        WeaponFX
-- @version     1.0
-- @description Local viewmodel shaders (Weapon, Gloves, Hands, Sleeves) driven from the Nixware menu.

-- WeaponFX (MIT, Garagoro/WeaponFX) draws custom HLSL shaders on the local
-- viewmodel only. Place WeaponFX.dll and the shaders/ folder in:
--   C:\nix\scripts\WeaponFX\
-- Custom .hlsl shaders dropped into C:\nix\scripts\WeaponFX\shaders\ are
-- picked up automatically on script load.

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
uint32_t GetEnvironmentVariableA(const char* name, char* buffer, uint32_t size);
uint32_t GetFileAttributesA(const char* path);
int      CopyFileA(const char* existing, const char* new_file, int fail_if_exists);
]]

local C = ffi.C
local DLL_NAME = 'WeaponFX.dll'

local function env(name)
    local buf = ffi.new('char[1024]')
    local n = C.GetEnvironmentVariableA(name, buf, 1024)
    if n == 0 or n >= 1024 then return nil end
    return ffi.string(buf, n)
end

local function file_exists(path)
    return path ~= nil and path ~= '' and C.GetFileAttributesA(path) ~= 0xFFFFFFFF
end

local function resolve_data_dir()
    local from_env = env('WEAPONFX_DIR')
    if file_exists(from_env) then return from_env end
    local nix_dir = 'C:\\nix\\scripts\\WeaponFX'
    if file_exists(nix_dir) then return nix_dir end
    return (env('LOCALAPPDATA') or '') .. '\\WeaponFX'
end

local DATA_DIR = resolve_data_dir()
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

-- UI (2 columns: Left = Weapon & Hands, Right = Gloves & Sleeves) -----------

local dirty = true
local function mark_dirty() dirty = true end

local tab = assert(ui.tab('weaponfx', 'WeaponFX'))

local TARGET_SPECS = {
    { prefix = 'weapon',  label = 'Weapon',  section = 'default', side = 'left',  default_on = true,  bottom_pad = true  },
    { prefix = 'gloves',  label = 'Gloves',  section = 'gloves',  side = 'right', default_on = false, bottom_pad = true  },
    { prefix = 'hands',   label = 'Hands',   section = 'hands',   side = 'left',  default_on = false, bottom_pad = false },
    { prefix = 'sleeves', label = 'Sleeves', section = 'sleeves', side = 'right', default_on = false, bottom_pad = false },
}

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
for _, spec in ipairs(TARGET_SPECS) do
    local prefix = spec.prefix
    local sec = tab:section(prefix .. '_sec', spec.label, spec.side)

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

    if spec.bottom_pad then
        sec:separator()
        sec:label(' ')
    end

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
local pending_reload = false
local next_try = 0
local staged_mtime = nil

local function file_mtime(path)
    local data = ffi.new('WFX_FIND_DATAA')
    local h = C.FindFirstFileA(path, data)
    if ffi.cast('intptr_t', h) == -1 then return nil end
    C.FindClose(h)
    return ('%08X:%08X'):format(data.ftLastWriteTime.hi, data.ftLastWriteTime.lo)
end

local function module_handle()
    local h = C.GetModuleHandleA(DLL_NAME)
    if h == nil then return nil end
    return h
end

local STAGED_DLL = '' -- dev-only: path to a freshly built DLL; empty in release

local function apply_staged_dll(target_path)
    local staged = file_exists(STAGED_DLL) and STAGED_DLL or (target_path .. '.new')
    local m = file_mtime(staged)
    if not m or m == staged_mtime then return false end
    if C.CopyFileA(staged, target_path, 0) ~= 0 then
        staged_mtime = m
        log('updated ' .. target_path)
        return true
    end
    return false
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
    log('loaded ' .. DLL_PATH)
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
        local now = os.clock()
        if now < next_try then return end
        next_try = now + 1
        local loaded = module_handle() ~= nil
        local staged = file_exists(STAGED_DLL) and STAGED_DLL or (DLL_PATH .. '.new')
        local m = file_mtime(staged)
        if not staged_mtime then staged_mtime = file_mtime(DLL_PATH) or m end
        if m and m ~= staged_mtime then
            if loaded then
                if not pending_reload then
                    pending_reload = true
                    stop_dll()
                end
                return
            end
            apply_staged_dll(DLL_PATH)
            pending_reload = false
            load_failed = false
        end
        if not loaded and not load_failed then
            load_dll()
        end
    end)
    if not ok then log(tostring(err)) end
end)

events.shutdown(function()
    pcall(stop_dll)
end)
