import Foundation

@main struct ProfileTests {
    static func main() throws {
        let decoder = JSONDecoder()
        let s = try decoder.decode(Sample.self, from: Data(#"{"t":1,"fan_rpm":[1300,0,1400],"gpu_profile":{"gpu_id":256,"tach_slot_mask":5,"thermal_target_mask":3,"power_read_mask":7}}"#.utf8))
        precondition(s.gpu_profile?.tachSlots == [0, 2])
        precondition(s.gpu_profile?.thermalNames == ["GPU", "VRAM"])
        precondition(s.gpu_profile?.powerNames == ["Límite", "Media", "Instantánea"])
        let old = try decoder.decode(Sample.self, from: Data(#"{"t":1}"#.utf8))
        precondition(old.gpu_profile == nil)
        print("PASS: GPU profile, sparse tach slots and older agent compatibility")
    }
}
