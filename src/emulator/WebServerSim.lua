-- FCEUX HIGH-SPEED MODEM SIMULATION
local REQUEST_ID = 0x01
local currentRequestBit = 0
local responseByte = 0
local responseBitCount = 0
local lastClock = 0
local stringBuffer = ""
local stringLength = -1

-- 1. SENDING DATA TO NES (Request Phase)
-- Every time the NES reads $4016, we provide the next bit of the ID
memory.registerread(0x4016, function(address, value)
    local bitIndex = currentRequestBit % 8
    local bitValue = (bit.shift(REQUEST_ID, -bitIndex)) % 2
    
    local joy = joypad.get(1)
    if bitValue == 1 then joy.A = true else joy.A = false end
    joypad.set(1, joy)
    
    currentRequestBit = currentRequestBit + 1
end)

-- 2. RECEIVING DATA FROM NES (Response Phase)
memory.registerwrite(0x4016, function(address, value)
    local dataBit = value % 2 -- Bit 0
    local clockBit = math.floor(value / 2) % 2 -- Bit 1

    -- Detect Strobe (Reset)
    if value == 1 then
        currentRequestBit = 0
        print("ARDUINO: Strobe! Preparing to send ID: " .. REQUEST_ID)
    end

    -- Detect Clock Rising Edge
    if clockBit == 1 and lastClock == 0 then
        -- We shift bits in from the left (LSB first)
        responseByte = math.floor(responseByte / 2) + (dataBit * 128)
        responseBitCount = responseBitCount + 1
        
        if responseBitCount == 8 then
            if stringLength == -1 then
                stringLength = responseByte
                stringBuffer = ""
                print("ARDUINO: Received Length: " .. stringLength)
            else
                stringBuffer = stringBuffer .. string.char(responseByte)
                stringLength = stringLength - 1
                if stringLength == 0 then
                    print("ARDUINO: RESPONSE: [" .. stringBuffer .. "]")
                    stringLength = -1
                end
            end
            responseBitCount = 0
            responseByte = 0
        end
    end
    lastClock = clockBit
end)

while true do
    emu.frameadvance()
end
