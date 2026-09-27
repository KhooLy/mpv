local overlay_id = 63

local osd = mp.create_osd_overlay("ass-events")
local req = nil
local busy = false
local dirty = false
local exact = true
local shown = false

local function clear()
    req = nil
    dirty = false
    osd:remove()
    if shown then
        mp.commandv("overlay-remove", overlay_id)
        shown = false
    end
end

local function request()
    if busy or not req then
        return
    end
    busy = true
    dirty = false
    local r = req
    mp.command_native_async({"thumbnail", r["hover-sec"], overlay_id, r.x, r.y, r.w, r.h},
        function(ok, res)
            busy = false
            if req ~= r then
                if not req then
                    mp.commandv("overlay-remove", overlay_id)
                    shown = false
                end
                request()
                return
            end
            shown = shown or ok
            exact = ok and res.exact
            if dirty then
                request()
            end
        end)
end

mp.observe_property("user-data/osc/draw-preview", "native", function(_, value)
    if not value or not mp.get_property_native("thumbnail-info") then
        clear()
        return
    end
    req = value
    if value.ass then
        osd.data = value.ass
        osd:update()
    end
    dirty = true
    request()
end)

mp.observe_property("osd-dimensions", "native", function(_, dim)
    osd.res_x = dim.w
    osd.res_y = dim.h
    if req and req.ass then
        osd:update()
    end
end)

mp.observe_property("thumbnail-info", "native", function(_, value)
    if not value then
        clear()
    elseif req and not exact then
        dirty = true
        request()
    end
end)
