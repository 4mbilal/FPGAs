function h = find_homography(pts_in, pts_out)

alpha = inv([pts_in(1,1), pts_in(2,1), pts_in(3,1); pts_in(1,2), pts_in(2,2), pts_in(3,2); 1, 1, 1])*[pts_in(4,1);pts_in(4,2);1];
A = [alpha(1)*pts_in(1,1), alpha(2)*pts_in(2,1), alpha(3)*pts_in(3,1); alpha(1)*pts_in(1,2), alpha(2)*pts_in(2,2), alpha(3)*pts_in(3,2); alpha(1)*1, alpha(2)*1, alpha(3)*1];
alpha = inv([pts_out(1,1), pts_out(2,1), pts_out(3,1); pts_out(1,2), pts_out(2,2), pts_out(3,2); 1, 1, 1])*[pts_out(4,1);pts_out(4,2);1];
B = [alpha(1)*pts_out(1,1), alpha(2)*pts_out(2,1), alpha(3)*pts_out(3,1); alpha(1)*pts_out(1,2), alpha(2)*pts_out(2,2), alpha(3)*pts_out(3,2); alpha(1)*1, alpha(2)*1, alpha(3)*1];
h = B*inv(A);
h = h';
h = h/h(9);
