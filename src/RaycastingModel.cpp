/*
 * 6D localization for Micro Aerial Vehicle
 *
 * Copyright 20014-2015 Zheng Fang, Carnegie Mellon University
 * Email: fangzheng81@gmail.com
 *
 *
 */

#include <mav_localization/RaycastingModel.h>

#include <pcl/point_types.h>
#include <pcl/conversions.h>
#include <pcl_ros/transforms.h>
#include <octomap_ros/conversions.h>

namespace MAV_localization
{

RaycastingModel::RaycastingModel ( ros::NodeHandle* nh, boost::shared_ptr<MapModel> mapModel, EngineT * rngEngine ) :
    ObservationModel ( nh, mapModel, rngEngine )
{
    // params:
    nh->param ( "raycasting/z_hit", m_zHit, 0.8 );
    nh->param ( "raycasting/z_short", m_zShort, 0.1 );
    nh->param ( "raycasting/z_max", m_zMax, 0.05 );
    nh->param ( "raycasting/z_rand", m_zRand, 0.05 );
    nh->param ( "raycasting/sigma_hit", m_sigmaHit, 0.02 );
    nh->param ( "raycasting/lambda_short", m_lambdaShort, 0.1 );

    if ( m_zMax <= 0.0 )
    {
        ROS_ERROR ( "raycasting/z_max needs to be > 0.0" );
    }

    if ( m_zRand <= 0.0 )
    {
        ROS_ERROR ( "raycasting/z_rand needs to be > 0.0" );
    }

    virtual_cloud_pub = nh->advertise<sensor_msgs::PointCloud2> ( "virtual_cloud", 1 );
}

RaycastingModel::~RaycastingModel()
{

}

void RaycastingModel::integrateMeasurement ( Particles& particles, const PointCloud& pc, const std::vector<float>& ranges,
        float max_range, const tf::Transform& base_to_laser, int bestParticleIdx )
{
    assert ( pc.size() == ranges.size() );

    if ( !m_map )
    {
        ROS_ERROR ( "Map file is not set in raycasting" );
        return;
    }
    // iterate over samples, multi-threaded:
    //#pragma omp parallel for
    for ( unsigned i = 0; i < particles.size(); ++i )
    {
        Eigen::Matrix4f globalLaserOrigin;
        tf::Transform globalLaserOriginTf = particles[i].pose * base_to_laser;
        pcl_ros::transformAsMatrix ( globalLaserOriginTf, globalLaserOrigin );

        // raycasting origin
        octomap::point3d originP ( globalLaserOriginTf.getOrigin().x(), globalLaserOriginTf.getOrigin().y(),
                                   globalLaserOriginTf.getOrigin().z() );
        PointCloud pc_transformed;
        pcl::transformPointCloud ( pc, pc_transformed, globalLaserOrigin );


        int point_counter = 0;
        pcl::PointXYZ pt;
        PointCloud::Ptr virtual_cloud ( new PointCloud );
        virtual_cloud->header.frame_id = "/map";
        virtual_cloud->height = 1;
        virtual_cloud->width = 1;
        virtual_cloud->is_dense = true;
        if ( i == bestParticleIdx )
        {
            std::cout << "ray casting number is: " << pc_transformed.size() << std::endl;
        }
        // iterate over beams:
        PointCloud::const_iterator pc_it = pc_transformed.begin();
        std::vector<float>::const_iterator ranges_it = ranges.begin();
        for ( ; pc_it != pc_transformed.end(); ++pc_it, ++ranges_it )
        {

            double p = 0.0; // probability for weight

            if ( *ranges_it <= max_range )
            {

                // direction of ray in global (map) coords
                octomap::point3d direction ( pc_it->x, pc_it->y, pc_it->z );
                direction = direction - originP;

                // TODO: check first if endpoint is within map?
                octomap::point3d end;
                // raycast in OctoMap, we need to cast a little longer than max_range
                // to correct for particle drifts away from obstacles
                if ( m_map->castRay ( originP, direction, end, true, 1.5 * max_range ) )
                {
                    if ( i == bestParticleIdx )
                    {
                        pt.x = end.x();
                        pt.y = end.y();
                        pt.z = end.z();
                        point_counter++;
                        virtual_cloud->points.push_back ( pt );
                    }

                    //there are four different kinds of observations: known obstacle, unknown obstacle, random reading and maxrange
                    assert ( m_map->isNodeOccupied ( m_map->search ( end ) ) );
                    float raycastRange = ( originP - end ).norm();
                    float z = raycastRange - *ranges_it;
                    float sigma_scaled = m_sigmaHit;

                    if ( m_use_squared_error )
                        sigma_scaled = ( *ranges_it ) * ( *ranges_it ) * ( m_sigmaHit );

                    //part1: obstacle hit:
                    p += m_zHit / ( SQRT_2_PI * sigma_scaled ) * exp ( - ( z * z ) / ( 2 * sigma_scaled * sigma_scaled ) );

                    //part2: short range:
                    if ( *ranges_it <= raycastRange )
                        p += m_zShort * m_lambdaShort * exp ( -m_lambdaShort * ( *ranges_it ) );
                    // / (1 - exp(-m_lambdaShort * raycastRange));

                    //part3: random measurement:
                    p += m_zRand / max_range;
                }
                else
                {
                    // racasting did not hit, but measurement is no maxrange => random?
                    p += m_zRand / max_range;
                }

            }
            else
            {
                //part4: maximum range
                p += m_zMax;
            }

            // add log-likelihood
            // (note: likelihood can be larger than 1!)
            assert ( p > 0.0 );
            particles[i].weight += log ( p );

        } // end of loop over scan

        if ( i == bestParticleIdx )
        {
            virtual_cloud->width = point_counter;
            virtual_cloud_pub.publish ( virtual_cloud );
        }

    } // end of loop over particles

}

bool RaycastingModel::getHeightError ( const Particle& p, const tf::StampedTransform& footprintToBase,
                                       double& heightError ) const
{

    octomap::point3d direction = octomap::pointTfToOctomap ( footprintToBase.inverse().getOrigin() );
    octomap::point3d origin = octomap::pointTfToOctomap ( p.pose.getOrigin() );
    octomap::point3d end;
    // cast ray to bottom:
    if ( !m_map->castRay ( origin, direction, end, true, 2 * direction.norm() ) )
        return false;

    heightError = std::max ( 0.0,
                             std::abs ( ( origin - end ).z() - footprintToBase.getOrigin().z() ) - m_map->getResolution() );
    //ROS_INFO("Height error: %f", heightError);

    return true;
}

}
