// File: middleware/swarm_wci_dtn_node.cpp
// Standard: C++20 | Role: 6-Bot Cooperative WCI, GCC-AKF, DP Scheduler & CCSDS DTN Node

#include <iostream>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <string>

namespace qsnasb::middleware {

using Mat3 = std::array<std::array<double, 3>, 3>;
using Vec3 = std::array<double, 3>;

struct BotNeighborMeasurement {
    int bot_id{};
    Vec3 relative_pos_m{};
    Mat3 cov_nominal{};
    double snr_db{24.0};
    double innovation_norm_m{0.15};
    double gdop{1.2};
    bool link_active{true};
};

// Phase 3: Quality-Aware Logical AND-Gate Neighbor Filter
class QualityAwareAndGateFilter {
public:
    static bool admit_neighbor(const BotNeighborMeasurement& meas,
                               double min_snr_db = 12.0,
                               double max_innovation_m = 2.5,
                               double max_gdop = 4.5) noexcept 
    {
        if (!meas.link_active) return false;
        bool f_snr = (meas.snr_db >= min_snr_db);
        bool f_innov = (meas.innovation_norm_m <= max_innovation_m);
        bool f_geom = (meas.gdop <= max_gdop);
        // Logical AND-Gate: exclude neighbor if any operational quality factor fails
        return f_snr && f_innov && f_geom;
    }
};

// Phase 4: Subspace-Weighted Covariance Intersection (WCI) + Group Covariance Compensated AKF
class WeightedCovarianceIntersectionEngine {
private:
    Mat3 subspace_weights_W_{{{1.0, 0.0, 0.0},
                              {0.0, 1.0, 0.0},
                              {0.0, 0.0, 0.25}}}; // W = S^T * S subspace scaling

    static Mat3 invert_diagonal_3x3(const Mat3& A) noexcept {
        Mat3 inv{};
        for (size_t i = 0; i < 3; ++i) {
            inv[i][i] = (std::abs(A[i][i]) > 1e-12) ? (1.0 / A[i][i]) : 1e12;
        }
        return inv;
    }

    static double weighted_trace(const Mat3& W, const Mat3& P) noexcept {
        double tr = 0.0;
        for (size_t i = 0; i < 3; ++i) {
            tr += W[i][i] * P[i][i];
        }
        return tr;
    }

public:
    // Optimizes omega* = argmin_{omega in (0,1)} Tr(S^T S P(omega))
    double optimize_wci_weight(const Mat3& P_local_prior, const Mat3& R_neighbor_corr) const noexcept {
        Mat3 P_inv_1 = invert_diagonal_3x3(P_local_prior);
        Mat3 P_inv_2 = invert_diagonal_3x3(R_neighbor_corr);

        double best_omega = 0.5;
        double min_cost = 1e30;

        for (int step = 1; step < 100; ++step) {
            double omega = static_cast<double>(step) / 100.0;
            Mat3 P_fused_inv{};
            for (size_t i = 0; i < 3; ++i) {
                P_fused_inv[i][i] = omega * P_inv_1[i][i] + (1.0 - omega) * P_inv_2[i][i];
            }
            Mat3 P_fused = invert_diagonal_3x3(P_fused_inv);
            double cost = weighted_trace(subspace_weights_W_, P_fused);
            if (cost < min_cost) {
                min_cost = cost;
                best_omega = omega;
            }
        }
        return best_omega;
    }

    // Performs WCI update and applies Group Covariance Compensation (GCC) for AKF
    Mat3 fuse_and_compensate_akf(const Mat3& P_local_prior,
                                 const Mat3& R_neighbor_corr,
                                 const Vec3& innovation_vec,
                                 double& omega_out,
                                 Mat3& akf_estimated_R_out) const noexcept 
    {
        omega_out = optimize_wci_weight(P_local_prior, R_neighbor_corr);
        Mat3 P_inv_1 = invert_diagonal_3x3(P_local_prior);
        Mat3 P_inv_2 = invert_diagonal_3x3(R_neighbor_corr);

        Mat3 P_wci_inv{};
        for (size_t i = 0; i < 3; ++i) {
            P_wci_inv[i][i] = omega_out * P_inv_1[i][i] + (1.0 - omega_out) * P_inv_2[i][i];
        }
        Mat3 P_wci = invert_diagonal_3x3(P_wci_inv);

        // Resolve CI-AKF Blindness Paradox:
        // CI inflates P_wci by factor ~ 1/omega_out. Without GCC rescaling,
        // subtracting P_wci from innovation covariance underestimates observation noise R.
        double gcc_deinflation_factor = 2.0 * omega_out * (1.0 - omega_out) + 0.5;
        for (size_t i = 0; i < 3; ++i) {
            double empirical_innov_cov = innovation_vec[i] * innovation_vec[i];
            double compensated_prior = gcc_deinflation_factor * P_wci[i][i];
            akf_estimated_R_out[i][i] = std::max(1e-4, empirical_innov_cov - compensated_prior);
        }
        return P_wci;
    }
};

// Dynamic Programming (DP) Reactive Imaging Scheduler (55-deg Field of Regard)
struct GroundGridPoint {
    int gp_id{};
    double off_nadir_angle_deg{};
    double science_hazard_value{};
    double timestamp_s{};
};

class ReactiveImagingScheduler {
public:
    static constexpr double MAX_FOR_ANGLE_DEG = 55.0; // 5x nadir distortion limit
    static constexpr double SLEW_RATE_DEG_PER_S = 2.5;

    static std::vector<int> compute_optimal_schedule(
        const std::vector<GroundGridPoint>& candidate_gps) 
    {
        // Filter points inside the 55-deg Field of Regard
        std::vector<GroundGridPoint> valid_gps;
        for (const auto& gp : candidate_gps) {
            if (std::abs(gp.off_nadir_angle_deg) <= MAX_FOR_ANGLE_DEG) {
                valid_gps.push_back(gp);
            }
        }
        size_t n = valid_gps.size();
        if (n == 0) return {};

        std::vector<double> dp_val(n, 0.0);
        std::vector<int> parent(n, -1);

        for (size_t i = 0; i < n; ++i) {
            dp_val[i] = valid_gps[i].science_hazard_value;
            for (size_t j = 0; j < i; ++j) {
                double dt = valid_gps[i].timestamp_s - valid_gps[j].timestamp_s;
                double d_angle = std::abs(valid_gps[i].off_nadir_angle_deg -
                                          valid_gps[j].off_nadir_angle_deg);
                double required_slew_time = d_angle / SLEW_RATE_DEG_PER_S;
                if (dt >= required_slew_time) {
                    if (dp_val[j] + valid_gps[i].science_hazard_value > dp_val[i]) {
                        dp_val[i] = dp_val[j] + valid_gps[i].science_hazard_value;
                        parent[i] = static_cast<int>(j);
                    }
                }
            }
        }

        // Backtrack optimal DP path and discard suboptimal branches
        auto max_it = std::max_element(dp_val.begin(), dp_val.end());
        int curr = static_cast<int>(std::distance(dp_val.begin(), max_it));
        std::vector<int> optimal_path;
        while (curr != -1) {
            optimal_path.push_back(valid_gps[curr].gp_id);
            curr = parent[curr];
        }
        std::reverse(optimal_path.begin(), optimal_path.end());
        return optimal_path;
    }
};

// CubeSat QKD (100 MHz Weak Coherent Pulse + HDBC T&S) & CCSDS DTN Link Calculator
struct CubeSatQkdDtnLink {
    static double compute_pass_secret_key_mbits(double elevation_deg,
                                                double pass_duration_s = 340.0) noexcept 
    {
        if (elevation_deg < 20.0) return 0.0; // Link unavailable below 20-deg mask
        double rad = elevation_deg * (3.141592653589793 / 180.0);
        // 1.99 Mbits distilled over 340 s pass at 100 MHz repetition rate
        return 1.99 * (pass_duration_s / 340.0) * std::sin(rad);
    }
};

} // namespace qsnasb::middleware

int main() {
    using namespace qsnasb::middleware;

    WeightedCovarianceIntersectionEngine wci_engine{};
    Mat3 P_local{{{0.22, 0.0, 0.0}, {0.0, 0.25, 0.0}, {0.0, 0.0, 0.08}}};
    Mat3 R_bot2{{{0.14, 0.0, 0.0}, {0.0, 0.16, 0.0}, {0.0, 0.0, 0.12}}};

    std::vector<BotNeighborMeasurement> swarm_inputs = {
        {1, {12400.0, 150.0, -30.0}, R_bot2, 26.5, 0.12, 1.1, true},
        {4, {48200.0, 999.0, 850.0}, R_bot2,  6.2, 9.85, 6.1, true} // Degraded / Spoofed outlier
    };

    std::cout << "=== Q-SNASB LAYER 2 SWARM WCI, GCC-AKF & DTN MIDDLEWARE ===\n";
    for (const auto& m : swarm_inputs) {
        bool admitted = QualityAwareAndGateFilter::admit_neighbor(m);
        std::cout << "Bot-" << m.bot_id << " AND-Gate Validation : "
                  << (admitted ? "ADMITTED" : "REJECTED (ANOMALY ISOLATED)") << "\n";
        if (admitted) {
            double omega_opt = 0.0;
            Mat3 akf_R{};
            Mat3 P_fused = wci_engine.fuse_and_compensate_akf(
                P_local, m.cov_nominal, {0.35, 0.28, 0.15}, omega_opt, akf_R);
            std::cout << "  -> Optimal WCI Weight omega* : " << std::fixed << std::setprecision(3)
                      << omega_opt << "\n";
            std::cout << "  -> Fused Covariance P_xx     : " << P_fused[0][0] << " m^2\n";
            std::cout << "  -> GCC-AKF Corrected R_xx    : " << akf_R[0][0] << " m^2\n";
        }
    }

    std::vector<GroundGridPoint> targets = {
        {101, 15.0, 8.5,   0.0},
        {102, 48.0, 9.2,  25.0},
        {103, 62.0, 99.0, 40.0}, // Outside 55-deg FOR -> automatically pruned
        {104, 22.0, 14.0, 50.0}
    };
    auto schedule = ReactiveImagingScheduler::compute_optimal_schedule(targets);
    std::cout << "DP Reactive Schedule (FOR<=55d): [ ";
    for (int id : schedule) std::cout << "GP-" << id << " ";
    std::cout << "]\n";

    std::cout << "CubeSat QKD Distilled Key (65d): "
              << CubeSatQkdDtnLink::compute_pass_secret_key_mbits(65.0) << " Mbits\n";
    return 0;
}