---
ns: CFX
apiset: client
game: rdr3
---
## GET_VEHICLE_ADDITIONAL_PROP_SET_HASH

```c
int GET_VEHICLE_ADDITIONAL_PROP_SET_HASH(Vehicle vehicle);
```

Gets the additional prop set hash for the specified vehicle.

This function reads the additional prop set hash from the vehicle's memory at offset 0xD90. This value is used to identify which additional prop set is applied to the vehicle.

## Parameters
* **vehicle**: The vehicle's entity handle.

## Return value
Returns the additional prop set hash as a uint32 value, or 0 if the vehicle is invalid.

## Examples
```lua
local vehicle = GetVehiclePedIsIn(PlayerPedId(), false)
if vehicle ~= 0 then
    local propSetHash = GET_VEHICLE_ADDITIONAL_PROP_SET_HASH(vehicle)
    print("Additional Prop Set Hash: " .. propSetHash)
end
```

## Notes
- This native reads directly from vehicle memory at offset 0xD90
- The vehicle must be a valid entity and must be of type CVehicle
- Returns 0 if the vehicle handle is invalid or the entity is not a vehicle
- The hash value can be used to identify specific prop sets applied to vehicles
