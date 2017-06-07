/*
 * Copyright (c) 2017 Ultimaker B.V.
 *
 * CuraEngine is released under the terms of the AGPLv3 or higher.
 */
#include <cassert>
#include <cmath>
#include <cstdlib>

#include <limits>
#include <algorithm>
#include <vector>

#ifndef NDEBUG
# include <iostream>
#endif // NDEBUG

#include "FermatSpiralMST.h"
#include "math.h"

using namespace cura;

#define DEFAULT_INFILL_WIDTH    300

//
// local data structures and ata
//
struct ConnectionSorter
{
    inline bool operator() (const struct SpiralContourNodeConnection * conn1, const struct SpiralContourNodeConnection * conn2)
    {
        return (conn1->weight < conn2->weight);
    }
};

//
// local functions
//
static bool shouldIncludeCij(const ClipperLib::IntPoint& cij,
                             struct SpiralContourNode *cip1j,
                             struct SpiralContourNode *cip1k);

static void reverseArcList(std::vector<struct Arc *>& result_list,
                           const std::vector<struct Arc *>& original_list);

static void chopOffPathOnPoints(const ClipperLib::Path& original_path,
                                const ClipperLib::IntPoint& new_p1,
                                const ClipperLib::IntPoint& new_p2,
                                int64_t chopoff_p1_idx,
                                int64_t chopoff_p2_idx,
                                ClipperLib::Path& result_path);


SpiralContourTree::SpiralContourTree()
    : m_tree_root(nullptr)
    , m_infill_width(DEFAULT_INFILL_WIDTH)
{
    this->m_contour_node_list.clear();
    this->m_all_node_connection_list.clear();
}


SpiralContourTree::~SpiralContourTree()
{
    // safely clear everything
    clear();
}

/*
 * Safely clears everything and releases all the allocated memories.
 */
void SpiralContourTree::clear()
{
    m_tree_root = nullptr;

    // clear all connection nodes
    for (auto itr_conn = m_all_node_connection_list.begin(); itr_conn != m_all_node_connection_list.end(); ++itr_conn)
    {
        // delete all arcs
        for (auto itr_arc = (*itr_conn)->arc_list.begin(); itr_arc != (*itr_conn)->arc_list.end(); ++itr_arc)
        {
            delete (*itr_arc);
        }
        delete (*itr_conn);
    }
    m_all_node_connection_list.clear();

    // clear all tree nodes
    for (uint32_t i = 0; i < m_all_contour_node_list.size(); ++i)
    {
        delete m_all_contour_node_list[i];
    }
    m_all_contour_node_list.clear();

    m_contour_node_list.clear();
}


void SpiralContourTree::connectContours(struct SpiralContourNode *node)
{
    if (node == nullptr)
    {
        node = this->m_tree_root;
    }

    // if this is not a parent node, handle the children nodes first and then connect
    bool did_connect = false;
    for (auto itr_conn = node->to_child_connection_list.begin(); itr_conn != node->to_child_connection_list.end(); ++itr_conn)
    {
        this->connectContours((*itr_conn)->child_node);
        did_connect = true;
    }

    if (node != this->m_tree_root)
    {
        // connect this node with its parent
        this->formPath(node);
    }
}


void travel_along_path_from_point_for_length(
    ClipperLib::IntPoint& result_point,
    uint64_t& result_point_start_index,
    uint64_t& result_point_end_index,
    const ClipperLib::Path& path,
    const ClipperLib::IntPoint& start_point,
    uint64_t start_index,
    double distance)
{
    double distance_to_travel = distance;
    struct ClipperLib::IntPoint p1 = start_point;
    struct ClipperLib::IntPoint p2 = path[start_index + 1];
    uint64_t current_index = start_index;

    while (true)
    {
        double remaining_distance_on_edge = p2p_dist(p1, p2);
        if (remaining_distance_on_edge >= distance_to_travel)
        {
            // get the point
            struct ClipperLib::IntPoint sp1 = path[current_index];
            struct ClipperLib::IntPoint sp2 = path[current_index + 1];
            double vec_x, vec_y;
            compute_unit_vector(vec_x, vec_y, sp1, sp2);

            double p_x = sp1.X + vec_x * remaining_distance_on_edge;
            double p_y = sp1.Y + vec_y * remaining_distance_on_edge;

            result_point.X = std::round(p_x);
            result_point.Y = std::round(p_y);

            result_point_start_index = current_index;
            result_point_start_index = current_index + 1;

            break;
        }

        distance_to_travel -= remaining_distance_on_edge;
        ++current_index;

        p1 = path[current_index];
        p2 = path[current_index + 1];
    }
}


void get_inward_vector(
    double& vec_x,
    double& vec_y,
    const ClipperLib::IntPoint& p1,
    const ClipperLib::IntPoint& p2,
    const ClipperLib::IntPoint& p3,
    int32_t direction)
{
    vec_x = ((p2.X - p1.X) + (p3.X - p2.X)) / 2;
    vec_y = ((p2.Y - p1.Y) + (p3.Y - p2.Y)) / 2;

    const double tmp = vec_x;
    vec_x = vec_y;
    vec_y = tmp;

    vec_x *= direction;
    vec_y *= -direction;
}


bool get_intersection_on_path(
    ClipperLib::IntPoint& intersection_point,
    uint64_t& start_index,
    uint64_t& end_index,
    double& distance,
    const ClipperLib::IntPoint& p1,
    const ClipperLib::IntPoint& p2,
    const ClipperLib::Path& path)
{
    bool found_intersection = false;
    ClipperLib::IntPoint point;
    double closest_distance = std::numeric_limits<double>::max();

    for (uint64_t i = 0; i < path.size(); ++i)
    {
        uint64_t j = i + 1;
        if (j >= path.size())
        {
            j = 0;
        }

        const ClipperLib::IntPoint& p3 = path[i];
        const ClipperLib::IntPoint& p4 = path[j];

        bool intersect = doIntersect(p1, p2, p3, p4);
        if (intersect)
        {
            compute_line_intersection(point, p1, p2, p3, p4);
            const double line_distance = p2p_dist(p1, point);

            if (line_distance < closest_distance)
            {
                closest_distance = line_distance;
                distance = closest_distance;
                intersection_point = point;
                start_index = i;
                end_index = j;
                found_intersection = true;
                break;
            }
        }
    }

    return found_intersection;
}


void chop_child_path(
    ClipperLib::Path& result_path,
    uint64_t pin_start_index,
    uint64_t pin_end_index,
    const ClipperLib::IntPoint& pin,
    uint64_t pout_start_index,
    uint64_t pout_end_index,
    const ClipperLib::IntPoint& pout,
    const ClipperLib::Path& path)
{
    // add pin
    result_path << pin;
    if (pin != path[pin_start_index] and pin != path[pin_end_index])
    {
        result_path << path[pin_end_index];
    }

    // add points in the middle: pin_end_index -> pout_start_index
    if (pin_end_index < pout_start_index)
    {
        for (uint64_t i = pin_end_index + 1; i < pout_start_index; ++i)
        {
            result_path << path[i];
        }
    }
    else
    {
        for (uint64_t i = pin_end_index + 1; i < path.size(); ++i)
        {
            result_path << path[i];
        }
        for (uint64_t i = 0; i < pout_start_index; ++i)
        {
            result_path << path[i];
        }
    }

    // add pout
    if (pout != path[pout_start_index] and pout != path[pout_end_index])
    {
        result_path << path[pout_start_index];
    }
    result_path << pout;
}


void SpiralContourTree::getInwardOutwardPoints(struct SpiralContourNodeConnection *connection)
{
    const double step_size = 50;  // TODO: make this configurable

    ClipperLib::IntPoint parent_inward_point;
    ClipperLib::IntPoint parent_outward_point;
    uint64_t parent_inward_point_start_idx = 0;
    uint64_t parent_inward_point_end_idx = 0;
    uint64_t parent_outward_point_start_idx = 0;
    uint64_t parent_outward_point_end_idx = 0;

    ClipperLib::IntPoint child_inward_point;
    ClipperLib::IntPoint child_outward_point;
    uint64_t child_inward_point_start_idx = 0;
    uint64_t child_inward_point_end_idx = 0;
    uint64_t child_outward_point_start_idx = 0;
    uint64_t child_outward_point_end_idx = 0;

    struct SpiralContourNode *parent_node = connection->parent_node;
    struct SpiralContourNode *child_node = connection->child_node;
    std::vector<struct Arc *> &arc_list = connection->arc_list;

    auto itr_arc = arc_list.begin();
    for (auto itr_arc = arc_list.begin(); itr_arc != arc_list.end(); ++itr_arc)
    {
        struct Arc *arc = *itr_arc;

        if (arc->length < m_infill_width)
        {
#ifndef NDEBUG
            std::cout << "arc length is smaller than infill width, skipping this arc."
                << " arc_length = " << arc->length << "; infill_width = " << m_infill_width << std::endl;
#endif // NDEBUG
            continue;
        }

        assert(arc->p1_index != arc->p2_index);
        double remaining_arc_length = arc->length;
        // find the suitable inward and outward points
        for (uint64_t pidx = arc->p1_index; pidx <= arc->p2_index; ++pidx)
        {
            const ClipperLib::IntPoint& inward_point = parent_node->path[pidx];
            uint64_t outward_point_start_index;
            uint64_t outward_point_end_index;
            ClipperLib::IntPoint outward_point;

            // get the outward point on parent
            travel_along_path_from_point_for_length(
                outward_point, outward_point_start_index, outward_point_end_index,
                parent_node->path, inward_point, pidx, m_infill_width);

            double inward_vector_x;
            double inward_vector_y;
            ClipperLib::IntPoint p1 = parent_node->path[(pidx == 0) ? (parent_node->path.size() - 1) : (pidx - 1)];
            ClipperLib::IntPoint p2 = parent_node->path[pidx];
            ClipperLib::IntPoint p3 = parent_node->path[pidx + 1];
            get_inward_vector(inward_vector_x, inward_vector_y, p1, p2, p3, parent_node->direction);

            ClipperLib::IntPoint inward_p2;
            inward_p2.X = inward_point.X + inward_vector_x * 10 * m_infill_width;
            inward_p2.Y = inward_point.Y + inward_vector_y * 10 * m_infill_width;

            // get intersection
            double inward_length;
            ClipperLib::IntPoint inward_intersection_point;
            uint64_t inward_intersection_point_start_index;
            uint64_t inward_intersection_point_end_index;
            bool intersect = get_intersection_on_path(
                inward_intersection_point,
                inward_intersection_point_start_index, inward_intersection_point_end_index,
                inward_length,
                inward_point, inward_p2, child_node->path);
            if (!intersect)
            {
                continue;
            }

            // use the same inward point to compute intersection point as the outward point on child
            ClipperLib::IntPoint outward_p2;
            outward_p2.X = outward_point.X + inward_vector_x * 10 * m_infill_width;
            outward_p2.Y = outward_point.Y + inward_vector_y * 10 * m_infill_width;

            double outward_length;
            ClipperLib::IntPoint outward_intersection_point;
            uint64_t outward_intersection_point_start_index;
            uint64_t outward_intersection_point_end_index;
            intersect = get_intersection_on_path(
                outward_intersection_point,
                outward_intersection_point_start_index, outward_intersection_point_end_index,
                outward_length,
                outward_point, outward_p2, child_node->path);
            if (!intersect)
            {
                continue;
            }

            // Make sure that the points are reasonable
            double longer_link_length = inward_length > outward_length ? inward_length : outward_length;
            double shorter_link_length = inward_length <= outward_length ? inward_length : outward_length;
            if (shorter_link_length * 1.10 >= longer_link_length)
            {
                std::cout << "link distance difference is too much, skipping" << std::endl;
                continue;
            }

            // we have found a path, now start connecting
            // TODO: chop off on child path
            ClipperLib::Path result_path;
            chop_child_path(
                result_path,
                inward_intersection_point_start_index,
                inward_intersection_point_end_index,
                inward_intersection_point,
                outward_intersection_point_start_index,
                outward_intersection_point_end_index,
                outward_intersection_point,
                child_node->path
            );

            /*
            this->chopOffPathOnPoints(child_node->path,
                inward_intersection_point, outward_intersection_point,
                inward_intersection_point_start_index, outward_intersection_point_end_index,
                result_path);
            */

            // TODO: chop off on parent path (complex)

            connection->processed = true;
        }

        if (connection->processed)
        {
            break;
        }
    }

    assert(connection->processed);
    if (!connection->processed)
    {
        // TODO: show error.
    }
}


void SpiralContourTree::generateFullPath(ClipperLib::Path& full_path, struct SpiralContourNode *node)
{
    if (node == nullptr)
    {
        node = this->m_tree_root;
    }

    uint64_t processed_conn_count = 0;
    bool all_conn_processed = processed_conn_count == node->to_child_connection_list.size();

    for (auto itr_pt = node->path.begin(); itr_pt != node->path.end(); ++itr_pt)
    {
        const ClipperLib::IntPoint& pt = *itr_pt;

        full_path << pt;

        if (all_conn_processed)
        {
            continue;
        }

        for (auto itr_conn = node->to_child_connection_list.begin(); itr_conn != node->to_child_connection_list.end(); ++itr_conn)
        {
            struct SpiralContourNodeConnection *conn = *itr_conn;
            if (conn->processed)
                continue;
            if (conn->inward_point_on_parent.X == pt.X and conn->inward_point_on_parent.Y == pt.Y)
            {
                // connect to the child and go further
                this->generateFullPath(full_path, conn->child_node);
                conn->processed = true;

                ++processed_conn_count;
                all_conn_processed = processed_conn_count == node->to_child_connection_list.size();
                break;
            }
        }
    }
}


/*
 * Forms a path from the given child node to its parent node.
 */
void SpiralContourTree::formPath(struct SpiralContourNode *child_node)
{
    struct SpiralContourNodeConnection* to_parent_connection = child_node->to_parent_connection_list[0];
    struct SpiralContourNode *parent_node = to_parent_connection->parent_node;
    std::vector<struct Arc *>& arc_list = to_parent_connection->arc_list;

    // try out all arcs
    bool path_formed = false;
    for (auto itr_arc = arc_list.begin(); itr_arc != arc_list.end(); ++itr_arc)
    {
        ClipperLib::IntPoint arc_start_point = (*itr_arc)->p1;
        ClipperLib::IntPoint arc_end_point = (*itr_arc)->p2;
        ClipperLib::Path arc_path;

        // get the path segment for this arc
        // a connection's arcs are on the parent node (outer spiral contour), so we need to find the path from the parent.
        assert(arc_start_point != arc_end_point);
        assert(is_point_in_path(arc_start_point, parent_node->path));
        assert(is_point_in_path(arc_end_point, parent_node->path));
        bool in_arc = false;
        for (auto itr_pt = parent_node->path.begin(); itr_pt != parent_node->path.end(); ++itr_pt)
        {
            if (!in_arc and *itr_pt == arc_start_point)
            {
                arc_path << *itr_pt;
                in_arc = true;
            }
            else if (in_arc)
            {
                arc_path << *itr_pt;
                if (*itr_pt == arc_end_point)
                {
                    in_arc = false;
                    break;
                }
            }
        }

        assert(arc_path.size() > 1);

        // -- find inward and outward points
        int64_t gap_distance = 5;  // TODO: make configurable

        // get inward and out points on parent contour
        int64_t inward_point_index = 0;

        int64_t outward_end_index = inward_point_index + 1;
        ClipperLib::IntPoint inward_point_on_parent = arc_path[inward_point_index];
        ClipperLib::IntPoint outward_point_on_parent = arc_path[outward_end_index];

        assert(inward_point_on_parent.X >= 0);
        assert(inward_point_on_parent.Y >= 0);
        assert(outward_point_on_parent.X >= 0);
        assert(outward_point_on_parent.Y >= 0);

        int64_t parent_chopoff_start_idx = get_point_idx_in_path(inward_point_on_parent, parent_node->path);
        int64_t parent_chopoff_end_idx = get_point_idx_in_path(arc_path[outward_end_index], parent_node->path);
        assert(parent_chopoff_start_idx >= 0);
        assert(parent_chopoff_end_idx >= 0);

        // get the inward and outward points on child contour
        ClipperLib::IntPoint inward_point_on_child;
        ClipperLib::IntPoint outward_point_on_child;
        uint64_t child_inward_chopoff_start_idx = 0;
        uint64_t child_inward_chopoff_end_idx = 0;
        uint64_t child_outward_chopoff_start_idx = 0;
        uint64_t child_outward_chopoff_end_idx = 0;

        get_closest_point_on_path_from_point(
            inward_point_on_child, child_inward_chopoff_start_idx,
            inward_point_on_parent, child_node->path);
        child_inward_chopoff_end_idx = child_inward_chopoff_start_idx - 1;

        get_closest_point_on_path_from_point(
            outward_point_on_child, child_outward_chopoff_start_idx,
            outward_point_on_parent, child_node->path);
        child_outward_chopoff_end_idx = child_outward_chopoff_start_idx - 1;

        // -- chop the path off on parent and child contours
        ClipperLib::Path result_parent_path;
        chopOffPathOnPoints(
            parent_node->path,
            inward_point_on_parent, outward_point_on_parent,
            parent_chopoff_start_idx, parent_chopoff_end_idx,
            result_parent_path);
        parent_node->path = result_parent_path;

        ClipperLib::Path result_child_path;
        chopOffPathOnPoints(
            child_node->path,
            inward_point_on_child, outward_point_on_child,
            child_inward_chopoff_start_idx, child_outward_chopoff_end_idx,
            result_child_path);
        child_node->path = result_child_path;

        assert(inward_point_on_child.X >= 0);
        assert(inward_point_on_child.Y >= 0);
        assert(outward_point_on_child.X >= 0);
        assert(outward_point_on_child.Y >= 0);

        // set inward and outward points for this connection
        to_parent_connection->inward_point_on_parent = inward_point_on_parent;
        to_parent_connection->outward_point_on_parent = outward_point_on_parent;
        to_parent_connection->inward_point_on_child = inward_point_on_child;
        to_parent_connection->outward_point_on_child = outward_point_on_child;

        // we have formed a connection, so no need to continue
        path_formed = true;
        break;
    }

    assert(path_formed);
}


void chopOffPathOnPoints(
    const ClipperLib::Path& original_path,
    const ClipperLib::IntPoint& new_p1,
    const ClipperLib::IntPoint& new_p2,
    int64_t chopoff_p1_idx,
    int64_t chopoff_p2_idx,
    ClipperLib::Path& result_path)
{
    ClipperLib::Path new_path;
    int64_t i;

    const ClipperLib::IntPoint& chopoff_p1 = original_path[chopoff_p1_idx];
    const ClipperLib::IntPoint& chopoff_p2 = original_path[chopoff_p2_idx];

    // if the chop off point is on the last edge, we need to create points
    if (chopoff_p1_idx >= chopoff_p2_idx)
    {
        result_path << new_p1;
        if (chopoff_p1 != new_p1)
        {
            result_path << chopoff_p1;
        }
        for (i = chopoff_p1_idx + 1; i < original_path.size(); ++i)
        {
            if (original_path[i].X == new_p2.X and original_path[i].Y == new_p2.Y)
                continue;
            if (original_path[i].X == new_p1.X and original_path[i].Y == new_p1.Y)
                continue;
            result_path << original_path[i];
        }
        for (i = 0; i < chopoff_p2_idx; ++i)
        {
            if (original_path[i].X == new_p2.X and original_path[i].Y == new_p2.Y)
                continue;
            if (original_path[i].X == new_p1.X and original_path[i].Y == new_p1.Y)
                continue;
            result_path << original_path[i];
        }
        if (chopoff_p2 != new_p2)
        {
            result_path << chopoff_p2;
        }
        result_path << new_p2;
    }
    else
    {
        result_path << new_p1;
        if (chopoff_p1 != new_p1)
        {
            result_path << chopoff_p1;
        }

        for (i = chopoff_p1_idx + 1; i < chopoff_p2_idx; ++i)
        {
            result_path << original_path[i];
        }

        if (chopoff_p2 != new_p2)
        {
            result_path << chopoff_p2;
        }
        result_path << new_p2;
    }
}


void SpiralContourTree::constructTree()
{
    if (m_contour_node_list.size() == 0)
    {
#ifdef NDEBUG
        std::cout << "no contour node to process." << std::endl;
#endif // NDEBUG
        return;
    }

#ifdef NDEBUG
    std::cout << "start constructing MST" << std::endl;
#endif // NDEBUG

    // generate connections between nodes
    uint32_t i;
    uint32_t j;
    uint32_t jp;
    uint32_t k;
    for (i = 0; i < m_contour_node_list.size(); ++i)
    {
        // if there is no next level, do nothing
        if (i + 1 >= m_contour_node_list.size())
        {
            break;
        }

        for (j = 0; j < m_contour_node_list[i].size(); ++j)
        {
            struct SpiralContourNode *c_ij = m_contour_node_list[i][j];

            // if there is only one contour, then there will only be one connection
            if (m_contour_node_list[i + 1].size() == 1)
            {
                struct SpiralContourNode *c_ip1_jp = m_contour_node_list[i + 1][0];
                this->computeConnections(c_ij, c_ip1_jp, nullptr);
                continue;
            }

            // get c[i+1,j'] and c[i+1,k]
            for (jp = 0; jp < m_contour_node_list[i + 1].size(); ++jp)
            {
                struct SpiralContourNode *c_ip1_jp = m_contour_node_list[i + 1][jp];
                for (k = 0; k < m_contour_node_list[i + 1].size(); ++k)
                {
                    if (k == jp)
                    {
                        continue;
                    }
                    struct SpiralContourNode *c_ip1_k = m_contour_node_list[i + 1][k];
                    this->computeConnections(c_ij, c_ip1_jp, c_ip1_k);
                }
            }
        }
    }

    // sort the connections based on weight
    std::vector<struct SpiralContourNodeConnection *> sorted_connection_list = m_all_node_connection_list;
    std::sort(sorted_connection_list.begin(), sorted_connection_list.end(), ConnectionSorter());

    // create a minimum spanning tree (MST)
    std::vector<struct SpiralContourNode *> already_connected_node_list;
    already_connected_node_list.reserve(this->m_all_contour_node_list.size());
    uint32_t created_connection_count = 0;

    for (auto itr_conn = sorted_connection_list.begin(); itr_conn != sorted_connection_list.end(); ++itr_conn)
    {
        // if all nodes have been connected, no need to continue
        if (created_connection_count == this->m_all_contour_node_list.size() - 1)
        {
            break;
        }

        struct SpiralContourNode *parent_node = (*itr_conn)->parent_node;
        struct SpiralContourNode *child_node = (*itr_conn)->child_node;

        // make sure we don't create a cyclic link
        bool found_parent = false;
        bool found_child = false;
        for (auto itr_connected_node = already_connected_node_list.begin(); itr_connected_node != already_connected_node_list.end(); ++itr_connected_node)
        {
            if ((*itr_connected_node) == parent_node)
            {
                found_parent = true;
            }
            else if ((*itr_connected_node) == child_node)
            {
                found_child = true;
            }
            if (found_parent and found_child)
            {
                break;
            }
        }
        // make sure we don't create a cyclic link
        if (found_parent and found_child and child_node->parent != nullptr)
        {
            continue;
        }

        // set this connection
        parent_node->to_child_connection_list.push_back(*itr_conn);
        this->updateNodeType(parent_node);
        child_node->to_parent_connection_list.push_back(*itr_conn);
        this->updateNodeType(child_node);
        child_node->parent = parent_node;
        ++created_connection_count;

        if (!found_parent)
        {
            already_connected_node_list.push_back(parent_node);
        }
        if (!found_child)
        {
            already_connected_node_list.push_back(child_node);
        }
    }

    // determines and sets the path directions of all contours
    this->determineContourDirections(this->m_tree_root, 0, false);

#ifndef NDEBUG
    //std::cout << ">>>>>>>>> MST:" << std::endl;
    //this->printMST(this->m_tree_root, 0);
#endif // NDEBUG
}


/*
 * This function is called after the tree is constructed. It uses depth-first search to
 * traverse through all the contours and determines the direction of all contours.
 */
int32_t SpiralContourTree::determineContourDirections(
    struct SpiralContourNode *node,
    int32_t parent_direction,
    bool is_parent_direction_set)
{
    bool direction_need_to_change = false; // whether the direction of this contour needs to be changed
    int32_t new_node_direction = 0; // the new direction if the direction of this contour needs to be changed
    bool is_node_direction_known = false; // whether the direction of this contour is known

    // if the parent's direction is known, we simple use the parent's direction to determine the child's direction.
    if (is_parent_direction_set)
    {
        new_node_direction = -parent_direction;
        is_node_direction_known = true;
        direction_need_to_change = node->direction != new_node_direction;
    }

    struct SpiralContourNodeConnection *connection = nullptr;
    for (auto itr_conn = node->to_child_connection_list.begin(); itr_conn != node->to_child_connection_list.end(); ++itr_conn)
    {
        connection = *itr_conn;
        int32_t child_direction = this->determineContourDirections(connection->child_node, new_node_direction, is_node_direction_known);

        // if the parent's direction has not been determined yet, we determine the direction of this node using the first child's direction
        if (!is_node_direction_known)
        {
            new_node_direction = -child_direction;
            is_node_direction_known = true;
            direction_need_to_change = node->direction != new_node_direction;
        }
    }

    if (!is_node_direction_known)
    {
        new_node_direction = node->direction;
        direction_need_to_change = false;
    }
#ifndef NDEBUG
    assert(new_node_direction != 0);
    if (direction_need_to_change)
    {
        assert(new_node_direction != node->direction);
    }
#endif // NDEBUG

    // reverse the paths and arcs if needed
    if (direction_need_to_change)
    {
        // reverse this path and set direction
        ClipperLib::Path reversed_path;
        reverse_path_direction(reversed_path, node->path);

        for (auto itr_pt = node->path.begin(); itr_pt != node->path.end(); ++itr_pt)
        {
            assert((*itr_pt).X >= 0);
            assert((*itr_pt).Y >= 0);
        }
        node->path = reversed_path;
        for (auto itr_pt = node->path.begin(); itr_pt != node->path.end(); ++itr_pt)
        {
            assert((*itr_pt).X >= 0);
            assert((*itr_pt).Y >= 0);
        }
        
        node->direction = new_node_direction;

        // we by now have reversed the contour path direction of this node.
        // because the arcs in the connections are ordered according to the parent node's direction,
        // in this case, we need to make sure that all child connections this node has need to be reversed too.
        for (auto itr_conn = node->to_child_connection_list.begin(); itr_conn != node->to_child_connection_list.end(); ++itr_conn)
        {
            connection = *itr_conn;

            std::vector<struct Arc *> reversed_arc_list;
            reverseArcList(reversed_arc_list, connection->arc_list);
            connection->arc_list = reversed_arc_list;
        }
    }

    assert(node->direction != 0);
    return node->direction;
}


void SpiralContourTree::addConnectionArc(struct SpiralContourNode *parent_node, struct SpiralContourNode *child_node, struct Arc *arc)
{
    struct SpiralContourNodeConnection *connection = nullptr;

    // find an existing connection object
    for (auto itr_conn = m_all_node_connection_list.begin(); itr_conn != m_all_node_connection_list.end(); ++itr_conn)
    {
        // TODO: check and optimise those checks
        if ((*itr_conn)->parent_node != parent_node)
        {
            continue;
        }
        if ((*itr_conn)->child_node != child_node)
        {
            continue;
        }

        connection = *itr_conn;
        break;
    }

    // create a new connection object if no existing can be found
    if (connection == nullptr)
    {
        connection = new struct SpiralContourNodeConnection;
        connection->parent_node = parent_node;
        connection->child_node = child_node;
        connection->weight = 0;
        connection->processed = false;
        connection->arc_list = std::vector<struct Arc *>();
        this->m_all_node_connection_list.push_back(connection);
    }

    connection->arc_list.push_back(arc);
    connection->weight += arc->point_count;  // weight is the number of points
}


void SpiralContourTree::addNode(struct SpiralContourNode *node, uint32_t level)
{
    // make sure that the node list for this level exists
    while (level >= m_contour_node_list.size())
    {
        m_contour_node_list.push_back(std::vector<struct SpiralContourNode *>());
    }

    // assign level number and node id and add to list
    node->level = level;
    node->index = m_contour_node_list[level].size();

    this->m_contour_node_list[level].push_back(node);
    this->m_all_contour_node_list.push_back(node);
}


void SpiralContourTree::setPolygons(const ClipperLib::Paths& paths)
{
    if (paths.size() == 0)
    {
        return;
    }

    // the first path is the outline, so we can directly feed the whole paths to the
    // handling function and it will generate spiral contour c[0,0] for the outline
    // path automatically.
    this->createNodes(0, paths);

    // set root node
    this->m_tree_root = this->m_contour_node_list[0][0];
}


void SpiralContourTree::updateNodeType(struct SpiralContourNode *node)
{
    uint32_t connection_count = node->to_parent_connection_list.size() + node->to_child_connection_list.size();
    uint32_t type = connection_count <= 2 ? 1 : 2;
    node->type = type;
}


/*
 * Creates child nodes of the given parent node and all child nodes for the child nodes this function has created.
 */
void SpiralContourTree::createNodes(uint32_t current_level, const ClipperLib::Paths& paths)
{
    for (auto itr_path = paths.begin(); itr_path != paths.end(); ++itr_path)
    {
        assert((*itr_path).size() > 1);

        // create a child node for each Polygon Path and continue
        struct SpiralContourNode *child_node = new struct SpiralContourNode;
        memset(child_node, 0, sizeof(struct SpiralContourNode));

        // create the child node
        child_node->path = *itr_path;

        for (auto itr_pt = child_node->path.begin(); itr_pt != child_node->path.end(); ++itr_pt)
        {
            assert((*itr_pt).X >= 0);
            assert((*itr_pt).Y >= 0);
        }

        child_node->paths << *itr_path;
        child_node->has_been_chopped = false;
        child_node->path_length = get_path_length(child_node->path);
        child_node->direction = compute_path_direction(child_node->path);
        child_node->parent = nullptr;
        child_node->to_child_connection_list = std::vector<struct SpiralContourNodeConnection *>();
        child_node->to_parent_connection_list = std::vector<struct SpiralContourNodeConnection *>();

        this->addNode(child_node, current_level);

        // create child nodes for this node (if any)
        ClipperLib::Paths child_node_paths;

        ClipperLib::ClipperOffset clipper(1.2, 10.0);
        clipper.AddPath(*itr_path, ClipperLib::jtMiter, ClipperLib::etClosedPolygon);
        clipper.MiterLimit = 1.2;
        clipper.Execute(child_node_paths, -this->m_infill_width);

        // create spiral contour tree nodes for this child node
        createNodes(current_level + 1, child_node_paths);
    }
}


static struct Arc *createArc(const ClipperLib::Path& path, uint64_t start_index, uint64_t end_index)
{
    assert(start_index != end_index);
    assert(end_index < path.size());

    struct Arc *arc = new struct Arc();
    arc->p1 = path[start_index];
    arc->p2 = path[end_index];
    arc->p1_index = start_index;
    arc->p2_index = end_index;

    arc->point_count = end_index - start_index + 1;
    arc->length = compute_path_length(path, start_index, end_index);

    assert(arc->p1 != arc->p2);
    assert(arc->point_count >= 2);
    assert(arc->length > 0);
    assert(arc->p1.X >= 0);
    assert(arc->p1.Y >= 0);
    assert(arc->p2.X >= 0);
    assert(arc->p2.Y >= 0);

    return arc;
}


void SpiralContourTree::computeConnections(
    struct SpiralContourNode *node_cij,
    struct SpiralContourNode *node_cip1j,
    struct SpiralContourNode *node_cip1k)
{
    bool is_in_nearest_area = false;
    uint64_t created_connection_count = 0;
    uint64_t nearest_area_start_point_index = 0;

    // if there is only one contour on the lower level, just take the whole contour as the nearest area
    if (node_cip1k == nullptr)
    {
        // add arc to the tree
        struct Arc *arc = createArc(node_cij->path, 0, node_cij->path.size() - 1);
        this->addConnectionArc(node_cij, node_cip1j, arc);
        return;
    }

    // take points on this node and compute distance towards the other
    uint64_t current_point_index = 0;
    for (auto itr_pt_cij = node_cij->path.begin(); itr_pt_cij != node_cij->path.end(); ++itr_pt_cij)
    {
        bool has_smallest_dj_prime = shouldIncludeCij(*itr_pt_cij, node_cip1j, node_cip1k);

        if (has_smallest_dj_prime)
        {
            if (!is_in_nearest_area)
            {
                // mark this point as the starting point of the current nearest area.
                is_in_nearest_area = true;
                nearest_area_start_point_index = current_point_index;
            }
        }
        else if (!has_smallest_dj_prime && is_in_nearest_area)
        {
            if (nearest_area_start_point_index < current_point_index - 1)
            {
                // add arc to the tree
                struct Arc *arc = createArc(node_cij->path, nearest_area_start_point_index, current_point_index - 1);
                this->addConnectionArc(node_cij, node_cip1j, arc);
                ++created_connection_count;
            }

            is_in_nearest_area = false;
        }

        ++current_point_index;
    }

    // if there is still an open nearest area, we need to conclude it.
    if (is_in_nearest_area)
    {
        if (nearest_area_start_point_index < current_point_index - 1)
        {
            // add arc to the tree
            struct Arc *arc = createArc(node_cij->path, nearest_area_start_point_index, current_point_index - 1);
            this->addConnectionArc(node_cij, node_cip1j, arc);
            ++created_connection_count;
        }
    }

    assert(created_connection_count > 0);
}


#ifndef NDEBUG
/*
 * Prints the minimum spanning tree (MST).
 */
void SpiralContourTree::printMST(const struct SpiralContourNode *node, uint32_t level)
{
    if (node == nullptr)
    {
        return;
    }

    for (uint32_t i = 0; i <= level; ++i)
    {
        std::cout << "-";
    }

    std::cout << "[" << node->level << "," << node->index << "]"
        << " t-" << node->type
        << " d= " << (node->direction > 0 ? "L" : "R")
        << std::endl;
    std::cout << " > to child connection list size = " << node->to_child_connection_list.size() << std::endl;

    for (auto itr_conn = node->to_child_connection_list.begin(); itr_conn != node->to_child_connection_list.end(); ++itr_conn)
    {
        const struct SpiralContourNodeConnection* connection = *itr_conn;
        printMST(connection->child_node, level + 1);
    }
}
#endif // NDEBUG


static bool shouldIncludeCij(
    const ClipperLib::IntPoint& cij,
    struct SpiralContourNode *cip1j,
    struct SpiralContourNode *cip1k)
{
    // no other contours on the same level as c[i+1,j].
    if (cip1k == nullptr)
    {
        return true;
    }

    double cip1j_closest_distance;
    double cip1k_closest_distance;

    bool found_cip1j = calculate_closest_distance_on_path_from_point(cip1j_closest_distance, cij, cip1j->path);
    bool found_cip1k = calculate_closest_distance_on_path_from_point(cip1k_closest_distance, cij, cip1k->path);
    if (!found_cip1j)
    {
        return false;
    }
    if (!found_cip1k)
    {
        return true;
    }
    bool has_smallest_dj_prime = cip1j_closest_distance < cip1k_closest_distance;

    return has_smallest_dj_prime;
}


static void reverseArcList(std::vector<struct Arc *>& result_list, const std::vector<struct Arc *>& original_list)
{
    result_list.reserve(original_list.size());
    for (auto itr_arc = original_list.rbegin(); itr_arc != original_list.rend(); ++itr_arc)
    {
        struct Arc *arc = *itr_arc;

        assert(arc->p1.X >= 0);
        assert(arc->p1.Y >= 0);
        assert(arc->p2.X >= 0);
        assert(arc->p2.Y >= 0);

        arc->p1.X ^= arc->p2.X;
        arc->p1.Y ^= arc->p2.Y;
        
        arc->p2.X ^= arc->p1.X;
        arc->p2.Y ^= arc->p1.Y;
        
        arc->p1.X ^= arc->p2.X;
        arc->p1.Y ^= arc->p2.Y;

        assert(arc->p1.X != arc->p2.X);
        assert(arc->p1.Y != arc->p2.Y);

        assert(arc->p1.X >= 0);
        assert(arc->p1.Y >= 0);
        assert(arc->p2.X >= 0);
        assert(arc->p2.Y >= 0);

        result_list.push_back(arc);
    }
}
